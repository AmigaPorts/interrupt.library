/* SPDX-License-Identifier: MPL-2.0 */
// Resident/vector layout and public ABI derived from rondoval's gic400.library
// under MPL-2.0. Native PC implementation; provenance is recorded in README.md.
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <hardware/intbits.h>
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase**)4UL)
#include <proto/exec.h>
#include <proto/devicetree.h>
#include <libraries/gic400.h>
#include "server.h"
#include "controller.h"
#include "dispatch.h"
#include "arrival.h"

#define IRQ_SPURIOUS 1023
#define IRQ_EDGE 1
#define IRQ_LOW 2
#define IRQ_RESERVED 4

struct PcInterruptState;

struct PcInterruptBase {
    struct Library library;
    struct PcInterruptState* state;
};

struct PcInterruptState {
    struct PcInterruptBase* bases[2];
    ULONG seglist, max_irqs, cpu_count, destination;
    struct Interrupt** handlers;
    struct Interrupt dispatcher, collector;
    struct LocalApic local_apic;
    struct InterruptVectors vectors;
    struct InterruptDomains domains;
    struct PendingInterrupts pending;
    struct InterruptArrival arrival;
    struct MessageInterrupts messages;
    UBYTE config[INTERRUPT_IRQ_LIMIT], routes[INTERRUPT_IRQ_LIMIT];
    unsigned wake_vector;
    bool wake_outstanding, dispatch_enabled;
};

// A library HUNK is not a Shell application. The linker keeps this entry first.
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
    return -1;
}

extern const UBYTE rom_end;
static const char library_name[] = LIBRARY_NAME;
static const char compatibility_name[] = "gic400.library";
static const char library_id[] = VERSION_STRING;
static const char compatibility_id[] = COMPATIBILITY_VERSION_STRING " interrupt.library alias";
static const char dispatcher_name[] = "PC interrupt dispatcher";
static struct Library* residentInit(ULONG seglist asm("a0"), struct ExecBase* exec_base asm("a6"));
const struct Resident interruptResident __attribute__((used, aligned(2))) = {
    RTC_MATCHWORD,
    (struct Resident*)&interruptResident,
    (APTR)&rom_end,
    RTF_COLDSTART,
    LIBRARY_VERSION,
    NT_LIBRARY,
    102,
    (APTR)library_name,
    (APTR)library_id,
    (APTR)residentInit
};

// Exclude native collection independently of Exec's software interrupt gate.
// SuperState keeps the active C stack and returns the supervisor stack to restore
// for a user caller. Nested supervisor callers get NULL and retain their mode.
struct InterruptLock {
    APTR user_state;
    UWORD sr;
};

static inline struct InterruptLock lockInterrupts(void)
{
    struct InterruptLock lock;
    lock.user_state = SuperState();
    __asm__ volatile("move.w sr,%0\n ori.w #0x0700,sr" : "=d"(lock.sr) : : "cc", "memory");
    return lock;
}

// SuperState's saved stack must be reinstated together with the user's CPU
// mask. Enabling interrupts before UserState would expose a supervisor return
// frame to Exec's IRQ exit and defer a task switch. A normal 68040 format-0
// return restores mode, mask, user stack and saved supervisor stack atomically.
void restoreUserInterrupts(APTR stack asm("d0"), UWORD sr asm("d1"));
__asm__(
    ".text\n"
    ".globl _restoreUserInterrupts\n"
    "_restoreUserInterrupts:\n"
    "movea.l (sp)+,a0\n"
    "move.l sp,usp\n"
    "movea.l d0,sp\n"
    "clr.w -(sp)\n"
    "move.l a0,-(sp)\n"
    "move.w d1,-(sp)\n"
    "rte\n"
);

static inline void unlockInterrupts(struct InterruptLock lock)
{
    if (lock.user_state) {
        restoreUserInterrupts(lock.user_state, lock.sr & ~0x2000);
    }
    else {
        __asm__ volatile("move.w %0,sr" : : "d"(lock.sr) : "cc", "memory");
    }
}

static LONG validate(struct PcInterruptBase* base, ULONG irq)
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    struct PendingInterruptState current;
    return getPendingInterruptState(&base->state->pending, irq, &current);
}

static LONG requestWake(struct PcInterruptState* state)
{
    if (state->wake_outstanding) {
        return 0;
    }

    LONG result;
    do {
        result = requestLocalApic(&state->local_apic, state->wake_vector);
    } while (result == GIC400_ERR_NOT_READY);
    // Retain the vector even before the accepted message becomes visible in IRR.
    if (!result) {
        state->wake_outstanding = TRUE;
    }

    return result;
}

static void wake(struct PcInterruptState* state)
{
    if (!state->dispatch_enabled || highestPendingInterrupt(&state->pending) < 0) {
        return;
    }

    if (requestWake(state)) {
        __builtin_trap();
    }
}

static LONG updateMask(struct PcInterruptState* state, ULONG irq)
{
    if (irq < INTERRUPT_SHARED_FIRST || !state->vectors.vector_for_irq[irq]) {
        return 0;
    }

    return updateCollectedInterruptMask(&state->arrival, irq);
}

static LONG bindSource(struct PcInterruptState* state, ULONG irq)
{
    if (irq < INTERRUPT_SHARED_FIRST) {
        return 0;
    }

    if (isMessageInterrupt(&state->messages, irq)) {
        unsigned vector = state->vectors.vector_for_irq[irq];
        return vector >= 32 && vector < 255 && state->vectors.irq_for_vector[vector] == irq ?
            0 : GIC400_ERR_NOT_READY;
    }

    if (state->config[irq] & IRQ_RESERVED) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    if (state->vectors.vector_for_irq[irq]) {
        return 0;
    }

    struct InterruptSource source;
    LONG result = findInterruptSource(&state->domains, irq, &source);

    if (result) {
        return result;
    }

    for (unsigned vector = 32; vector < 255; ++vector) {

        if (!(state->vectors.available[vector / 32] & (1UL << (vector & 31))) ||
            state->vectors.irq_for_vector[vector] != UINT16_MAX) {
            continue;
        }

        result = bindInterruptVector(&state->vectors, &state->local_apic, irq, vector);

        if (result) {
            continue;
        }

        result = routeIoApic(&source.domain->apic, source.pin, vector, state->destination,
            !!(state->config[irq] & IRQ_LOW), !(state->config[irq] & IRQ_EDGE));

        if (!result) {
            return 0;
        }

        releaseInterruptVector(&state->vectors, &state->local_apic, irq);
        return result;
    }

    return GIC400_ERR_NOT_READY;
}

static LONG configureSource(struct PcInterruptState* state, ULONG irq, UBYTE config)
{
    if (irq < INTERRUPT_SHARED_FIRST) {
        state->config[irq] = config;
        return 0;
    }

    if (isMessageInterrupt(&state->messages, irq)) {

        if (!(config & IRQ_EDGE) || (config & IRQ_LOW)) {
            return GIC400_ERR_NOT_ROUTABLE;
        }

        state->config[irq] = config;
        return bindSource(state, irq);
    }

    LONG result = bindSource(state, irq);

    if (result) {
        return result;
    }

    struct InterruptSource source;
    findInterruptSource(&state->domains, irq, &source);
    unsigned vector = state->vectors.vector_for_irq[irq];
    bool pending, active;
    localApicStatus(&state->local_apic, vector, &pending, &active);

    if (pending || active) {
        return GIC400_ERR_NOT_READY;
    }

    result = routeIoApic(&source.domain->apic, source.pin, vector, state->destination,
        !!(config & IRQ_LOW), !(config & IRQ_EDGE));

    if (!result) {
        state->config[irq] = config;
    }

    LONG mask_result = updateMask(state, irq);
    return result ? result : mask_result;
}

static LONG changeState(struct PcInterruptBase* base, ULONG irq, enum InterruptStateField field, unsigned value)
{
    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();
    result = bindSource(state, irq);

    if (!result) {
        struct PendingInterruptState previous;
        getPendingInterruptState(&state->pending, irq, &previous);
        result = setPendingInterruptState(&state->pending, irq, field, value);

        if (!result) {
            result = updateMask(state, irq);
        }

        if (result) {
            unsigned old = field == INTERRUPT_ENABLED ? previous.enabled :
                field == INTERRUPT_PENDING ? previous.pending :
                field == INTERRUPT_ACTIVE ? previous.active : previous.priority;
            setPendingInterruptState(&state->pending, irq, field, old);
            updateMask(state, irq);
        }
    }

    wake(state);
    unlockInterrupts(lock);
    return result;
}

static ULONG collect(struct PcInterruptBase* base asm("a1"))
{
    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();
    // The native CPU entry left the hardware ISR for this controller owner.
    // One notification can cover several retained ISR entries. Drain them in
    // hardware priority order, masking level sources before EOI. Device callback
    // completion rearms each source. Never skip or complete a foreign ISR.
    LONG result = collectInterruptArrivals(&state->arrival, state->wake_vector, &state->wake_outstanding);

    if (result < 0) {
        __builtin_trap();
    }

    unlockInterrupts(lock);
    return 0;
}

static ULONG dispatch(struct PcInterruptBase* base asm("a1"))
{
    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();
    LONG irq = state->dispatch_enabled ? takePendingInterrupt(&state->pending) : GIC400_ERR_NOT_FOUND;
    unlockInterrupts(lock);

    if (irq < 0) {
        return 0;
    }

    struct Interrupt* interrupt = state->handlers[irq];

    if (interrupt && interrupt->is_Code) {
        callInterruptServer(interrupt, irq, EXEC_BASE_NAME);
    }

    lock = lockInterrupts();
    LONG result = (ULONG)irq < INTERRUPT_SHARED_FIRST ? completePendingInterrupt(&state->pending, irq) :
        completeCollectedInterrupt(&state->arrival, irq);

    if (result) {
        __builtin_trap();
    }

    wake(state);
    unlockInterrupts(lock);
    return (ULONG)irq >= INTERRUPT_SHARED_FIRST;
}

static LONG addServer(ULONG irq asm("d0"), UBYTE priority asm("d1"), BOOL edge asm("d2"),
    struct Interrupt* interrupt asm("a1"), struct PcInterruptBase* base asm("a6"))
{
    if (!interrupt || !interrupt->is_Code) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();

    if (state->handlers[irq]) {
        result = state->handlers[irq] == interrupt ? 0 : GIC400_ERR_ALREADY_REGISTERED;
    }
    else {
        result = configureSource(state, irq, (state->config[irq] & ~IRQ_EDGE) | (edge ? IRQ_EDGE : 0));

        if (!result) {
            struct PendingInterruptState previous;
            getPendingInterruptState(&state->pending, irq, &previous);
            state->handlers[irq] = interrupt;
            setPendingInterruptState(&state->pending, irq, INTERRUPT_PRIORITY, priority);
            setPendingInterruptState(&state->pending, irq, INTERRUPT_ROUTED, 1);
            setPendingInterruptState(&state->pending, irq, INTERRUPT_ENABLED, 1);
            result = updateMask(state, irq);

            if (result) {
                state->handlers[irq] = NULL;
                setPendingInterruptState(&state->pending, irq, INTERRUPT_PRIORITY, previous.priority);
                setPendingInterruptState(&state->pending, irq, INTERRUPT_ROUTED, previous.routed);
                setPendingInterruptState(&state->pending, irq, INTERRUPT_ENABLED, previous.enabled);
                updateMask(state, irq);
            } else {
                state->routes[irq] = 1;
            }
        }
    }

    wake(state);
    unlockInterrupts(lock);
    return result;
}

static LONG remServer(ULONG irq asm("d0"), struct Interrupt* interrupt asm("a1"),
    struct PcInterruptBase* base asm("a6"))
{
    if (!interrupt) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();

    if (!state->handlers[irq]) {
        result = GIC400_ERR_NOT_FOUND;
    }
    else if (state->handlers[irq] != interrupt) {
        result = GIC400_ERR_INVALID_ARGUMENT;
    }
    else {
        result = changeState(base, irq, INTERRUPT_ENABLED, 0);

        if (!result) {
            state->handlers[irq] = NULL;

            if (irq >= INTERRUPT_SHARED_FIRST) {
                state->routes[irq] = 0;
                setPendingInterruptState(&state->pending, irq, INTERRUPT_ROUTED, 0);
            }
        }
    }

    unlockInterrupts(lock);
    return result;
}

static LONG getStatus(ULONG irq asm("d0"), BOOL* pending asm("a1"), BOOL* active asm("a2"),
    BOOL* enabled asm("a3"), struct PcInterruptBase* base asm("a6"))
{
    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    struct InterruptLock lock = lockInterrupts();
    struct PendingInterruptState current;
    getPendingInterruptState(&base->state->pending, irq, &current);

    if (pending) {
        *pending = current.pending;
    }

    if (active) {
        *active = current.active;
    }

    if (enabled) {
        *enabled = current.enabled;
    }

    unlockInterrupts(lock);
    return 0;
}

static LONG enableInt(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_ENABLED, 1);
}

static LONG disableInt(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_ENABLED, 0);
}

static LONG setPriority(ULONG irq asm("d0"), UBYTE priority asm("d1"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_PRIORITY, priority);
}

static LONG getPriority(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    LONG result = validate(base, irq);
    return result ? result : base->state->pending.priority[irq];
}

static LONG trigger(struct PcInterruptBase* base, ULONG irq, BOOL edge)
{
    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    struct InterruptLock lock = lockInterrupts();
    result = configureSource(base->state, irq, (base->state->config[irq] & ~IRQ_EDGE) | (edge ? IRQ_EDGE : 0));
    unlockInterrupts(lock);
    return result;
}

static LONG setEdge(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return trigger(base, irq, TRUE);
}

static LONG setLevel(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return trigger(base, irq, FALSE);
}

static LONG route(struct PcInterruptBase* base, ULONG irq, UBYTE cpu, BOOL enabled)
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    if (irq >= base->state->max_irqs) {
        return GIC400_ERR_INVALID_IRQ;
    }

    if (cpu >= 8) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (irq < INTERRUPT_SHARED_FIRST) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    if (cpu >= base->state->cpu_count) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    // The original emulator hands external IRQs to the 68k on CPU 0. Other
    // roles have no guest dispatcher; do not recreate native cross-CPU forwarding.
    if (cpu) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    struct InterruptLock lock = lockInterrupts();
    result = bindSource(base->state, irq);

    if (!result) {
        UBYTE previous = base->state->routes[irq];
        setPendingInterruptState(&base->state->pending, irq, INTERRUPT_ROUTED, enabled);
        result = updateMask(base->state, irq);

        if (!result) {
            base->state->routes[irq] = enabled;
        }
        else {
            setPendingInterruptState(&base->state->pending, irq, INTERRUPT_ROUTED, previous);
        }
    }

    wake(base->state);
    unlockInterrupts(lock);
    return result;
}

static LONG routeInt(ULONG irq asm("d0"), UBYTE cpu asm("d1"), struct PcInterruptBase* base asm("a6"))
{
    return route(base, irq, cpu, TRUE);
}

static LONG unrouteInt(ULONG irq asm("d0"), UBYTE cpu asm("d1"), struct PcInterruptBase* base asm("a6"))
{
    return route(base, irq, cpu, FALSE);
}

static LONG queryRoute(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    if (irq >= base->state->max_irqs) {
        return GIC400_ERR_INVALID_IRQ;
    }

    if (irq < INTERRUPT_SHARED_FIRST) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    LONG result = validate(base, irq);
    return result ? result : base->state->routes[irq];
}

static LONG setPending(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_PENDING, 1);
}

static LONG clearPending(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_PENDING, 0);
}

static LONG setActive(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_ACTIVE, 1);
}

static LONG clearActive(ULONG irq asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    return changeState(base, irq, INTERRUPT_ACTIVE, 0);
}

static LONG setMask(UBYTE mask asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    struct InterruptLock lock = lockInterrupts();
    setPendingInterruptMask(&base->state->pending, mask);
    wake(base->state);
    unlockInterrupts(lock);
    return 0;
}

static LONG getMask(struct PcInterruptBase* base asm("a6"))
{
    return base ? (LONG)base->state->pending.mask : GIC400_ERR_NOT_READY;
}

static LONG getRunning(struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    struct InterruptLock lock = lockInterrupts();
    struct PendingInterrupts* pending = &base->state->pending;
    LONG result = pending->depth ? pending->running_priority[pending->depth - 1] : 255;
    unlockInterrupts(lock);
    return result;
}

static LONG getHighest(struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    struct InterruptLock lock = lockInterrupts();
    LONG result = highestPendingInterrupt(&base->state->pending);
    unlockInterrupts(lock);
    return result == GIC400_ERR_NOT_FOUND ? IRQ_SPURIOUS : result;
}

static LONG getInfo(struct GICInfo* info asm("a1"), struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    if (!info) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    info->distributorIIDR = info->distributorTyper = info->cpuInterfaceIIDR = 0;
    info->maxIrqs = base->state->max_irqs;
    info->cpuCount = base->state->cpu_count;
    info->securityExtensions = info->lspiCount = 0;
    return 0;
}

static LONG setPolarity(ULONG irq asm("d0"), BOOL active_low asm("d1"), struct PcInterruptBase* base asm("a6"))
{
    LONG result = validate(base, irq);

    if (result) {
        return result;
    }

    if (active_low != FALSE && active_low != TRUE) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (irq < INTERRUPT_SHARED_FIRST) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();
    struct PendingInterruptState current;
    getPendingInterruptState(&state->pending, irq, &current);
    // Configure before registration, under the caller's Disable. This cannot
    // alter an existing owner's disabled source while looking for a free input.
    if (state->handlers[irq]) {
        result = GIC400_ERR_ALREADY_REGISTERED;
    }
    else if (current.enabled || current.pending || current.active ||
        (state->pending.running[irq / 32] & (1UL << (irq & 31)))) {
            result = GIC400_ERR_NOT_READY;
        }
    else {
        result = configureSource(state, irq, (state->config[irq] & ~IRQ_LOW) | (active_low ? IRQ_LOW : 0));
    }

    unlockInterrupts(lock);
    return result;
}

static LONG setDispatchEnabled(BOOL enabled asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    if (enabled != FALSE && enabled != TRUE) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct InterruptLock lock = lockInterrupts();
    base->state->dispatch_enabled = enabled;
    wake(base->state);
    unlockInterrupts(lock);
    return 0;
}

static LONG collectInterrupts(struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    struct InterruptLock lock = lockInterrupts();
    struct PcInterruptState* state = base->state;
    LONG result = collectInterruptArrivals(&state->arrival, state->wake_vector, &state->wake_outstanding);
    unlockInterrupts(lock);
    return result;
}

// Explicit OS notification, independent of callback eligibility. The caller
// retains its software work; collection owns EOI as for every existing wake.
static LONG requestWakeup(struct PcInterruptBase* base asm("a6"))
{
    if (!base) {
        return GIC400_ERR_NOT_READY;
    }

    struct InterruptLock lock = lockInterrupts();
    LONG result = requestWake(base->state);
    unlockInterrupts(lock);
    return result;
}

static void freeBase(struct PcInterruptBase* base)
{
    ULONG size = base->library.lib_NegSize + base->library.lib_PosSize;
    FreeMem((UBYTE*)base - base->library.lib_NegSize, size);
}

static void freeController(struct PcInterruptState* state)
{
    if (state->handlers) {
        FreeMem(state->handlers, INTERRUPT_IRQ_LIMIT * sizeof(*state->handlers));
    }

    if (state->domains.domains) {
        FreeMem((APTR)state->domains.domains, state->domains.count * sizeof(*state->domains.domains));
    }
}

static ULONG libExpunge(struct PcInterruptBase* base asm("a6"))
{
    Forbid();
    struct PcInterruptState* state = base->state;
    state->bases[0]->library.lib_Flags |= LIBF_DELEXP;
    state->bases[1]->library.lib_Flags |= LIBF_DELEXP;

    if (state->bases[0]->library.lib_OpenCnt || state->bases[1]->library.lib_OpenCnt) {
        Permit();
        return 0;
    }

    struct InterruptLock lock = lockInterrupts();

    if (state->messages.blocks || state->pending.depth || state->wake_outstanding ||
        highestPendingInterrupt(&state->pending) >= 0) {
        goto busy;
    }

    for (ULONG irq = INTERRUPT_SHARED_FIRST; irq < state->max_irqs; ++irq) {
        unsigned vector = state->vectors.vector_for_irq[irq];

        if (!vector) {
            continue;
        }

        struct InterruptSource source;
        findInterruptSource(&state->domains, irq, &source);

        if (maskIoApic(&source.domain->apic, source.pin, true)) {
            goto busy;
        }

        struct IoApicState hardware;
        ioApicState(&source.domain->apic, source.pin, &hardware);
        bool pending, active;
        localApicStatus(&state->local_apic, vector, &pending, &active);

        if (hardware.delivery_pending || hardware.remote_irr || pending || active) {
            goto busy;
        }
    }

    RemIntServer(INTB_EXTER, &state->dispatcher);
    RemIntServer(INTB_EXTER, &state->collector);
    unlockInterrupts(lock);
    ULONG seglist = state->seglist;
    Remove((struct Node*)state->bases[0]);
    Remove((struct Node*)state->bases[1]);
    freeController(state);
    freeBase(state->bases[0]);
    freeBase(state->bases[1]);
    FreeMem(state, sizeof(*state));
    Permit();
    return seglist;
busy:

    for (ULONG irq = INTERRUPT_SHARED_FIRST; irq < state->max_irqs; ++irq) {

        if (updateMask(state, irq)) {
            __builtin_trap();
        }
    }

    unlockInterrupts(lock);
    Permit();
    return 0;
}

static ULONG readCell(const UBYTE* value)
{
	return ((ULONG)value[0] << 24) | ((ULONG)value[1] << 16) | ((ULONG)value[2] << 8) | value[3];
}

static BOOL initController(struct PcInterruptState* state)
{
    APTR DeviceTreeBase = OpenResource("devicetree.resource");

    if (!DeviceTreeBase) {
        return FALSE;
    }

    APTR root = DT_OpenKey((CONST_STRPTR)"/");
    APTR platform = DT_OpenKey((CONST_STRPTR)"/emu68");
    BOOL ready = FALSE;

    if (!root || !platform) {
        goto done;
    }

    struct InterruptCpus cpus;
    uint32_t vectors[8];

    if (readInterruptCpus(DeviceTreeBase, platform, &cpus) || cpus.apic_ids[0] >= 255 ||
        readInterruptVectors(DeviceTreeBase, platform, 0, vectors)) {
        goto done;
    }

    state->cpu_count = cpus.count;
    state->destination = cpus.apic_ids[0];
    struct ApicWindow window;

    if (findApicWindow(DeviceTreeBase, root, APIC_LOCAL, NULL, &window) ||
        openLocalApic((APTR)window.address, window.size, &state->local_apic)) {
        goto done;
    }

    for (unsigned vector = 32; vector < 255; ++vector) {

        if (!(vectors[vector / 32] & (1UL << (vector & 31)))) {
            continue;
        }

        bool pending, active;
        localApicStatus(&state->local_apic, vector, &pending, &active);

        if (!pending && !active) {
            state->wake_vector = vector;
            break;
        }
    }

    if (!state->wake_vector) {
        goto done;
    }

    vectors[state->wake_vector / 32] &= ~(1UL << (state->wake_vector & 31));

    if (initInterruptVectors(vectors, &state->vectors)) {
        goto done;
    }

    ULONG count = 0;
    APTR previous = NULL;
    LONG result;

    while (!(result = findApicWindow(DeviceTreeBase, root, APIC_IO, previous, &window))) {
        previous = window.key;
        ++count;
    }

    if (result != GIC400_ERR_NOT_FOUND || !count || count > INTERRUPT_IRQ_LIMIT) {
        goto done;
    }

    struct IoApicDomain* domains = AllocMem(count * sizeof(*domains), MEMF_CLEAR);

    if (!domains) {
        goto done;
    }

    state->domains.domains = domains;
    state->domains.count = count;
    previous = NULL;

    for (ULONG i = 0; i < count; ++i) {

        if (findApicWindow(DeviceTreeBase, root, APIC_IO, previous, &window) ||
            openIoApic((APTR)window.address, window.size, &domains[i].apic) ||
            readIoApicGsiBase(DeviceTreeBase, root, platform, window.key, &domains[i].gsi_base)) {
            goto done;
        }

        domains[i].key = previous = window.key;
    }

    if (initInterruptDomains(domains, count, &state->domains)) {
        goto done;
    }

    state->max_irqs = state->domains.max_irqs;

    if (initMessageInterrupts(state->max_irqs, &state->messages)) {
        goto done;
    }

    uint32_t available[INTERRUPT_WORDS] = {0xffff0000};

    for (ULONG i = 0; i < count; ++i) {

        for (unsigned pin = 0; pin < domains[i].apic.input_count; ++pin) {
            ULONG irq = INTERRUPT_SHARED_FIRST + domains[i].gsi_base + pin;
            struct IoApicState hardware;

            if (ioApicState(&domains[i].apic, pin, &hardware)) {
                goto done;
            }

            available[irq / 32] |= 1UL << (irq & 31);
            state->config[irq] = IRQ_LOW; // Shared PCI wiring; clients may select polarity before registration.

            if (!hardware.masked || hardware.delivery_pending || hardware.remote_irr || hardware.delivery_mode ||
                hardware.logical_destination || (hardware.vector && (hardware.destination != state->destination ||
                !(vectors[hardware.vector / 32] & (1UL << (hardware.vector & 31)))))) {
                state->config[irq] |= IRQ_RESERVED;
            }
        }
    }

    APTR property = DT_FindProperty(platform, (CONST_STRPTR)"isa-interrupts");
    const UBYTE* isa = property ? DT_GetPropValue(property) : NULL;

    if (!isa || DT_GetPropLen(property) != 16 * 12) {
        goto done;
    }

    for (unsigned i = 0; i < 16; ++i) {
        ULONG gsi = readCell(isa + 12 * i + 4), sense = readCell(isa + 12 * i + 8);

        if (readCell(isa + 12 * i) != i || sense > 3) {
            goto done;
        }

        if (gsi >= state->max_irqs - INTERRUPT_SHARED_FIRST) {
            continue;
        }

        ULONG irq = INTERRUPT_SHARED_FIRST + gsi;
        state->config[irq] = (state->config[irq] & IRQ_RESERVED) |
            ((sense == 1 || sense == 3) ? IRQ_LOW : 0) | ((sense == 0 || sense == 3) ? IRQ_EDGE : 0);
    }

    property = DT_FindProperty(platform, (CONST_STRPTR)"nmi-interrupts");

    if (!property || DT_GetPropLen(property) % 4) {
        goto done;
    }

    const UBYTE* nmi = DT_GetPropValue(property);
    ULONG length = DT_GetPropLen(property);

    if (length && !nmi) {
        goto done;
    }

    for (ULONG i = 0; i < length; i += 4) {
        ULONG gsi = readCell(nmi + i);

        if (gsi < state->max_irqs - INTERRUPT_SHARED_FIRST) {
            ULONG irq = INTERRUPT_SHARED_FIRST + gsi;
            available[irq / 32] &= ~(1UL << (irq & 31));
        }
    }

    if (initPendingInterrupts(available, state->max_irqs, 127, &state->pending)) {
        goto done;
    }

    for (unsigned irq = 16; irq < 32; ++irq) {
        setPendingInterruptState(&state->pending, irq, INTERRUPT_ROUTED, 1);
    }

    state->handlers = AllocMem(INTERRUPT_IRQ_LIMIT * sizeof(*state->handlers), MEMF_CLEAR);

    if (!state->handlers) {
        goto done;
    }

    state->arrival = (struct InterruptArrival) { &state->domains, &state->vectors, &state->local_apic,
        &state->pending, &state->messages };
    state->dispatcher.is_Node.ln_Type = state->collector.is_Node.ln_Type = NT_INTERRUPT;
    state->dispatcher.is_Node.ln_Name = state->collector.is_Node.ln_Name = (char*)dispatcher_name;
    state->dispatcher.is_Data = state->collector.is_Data = state->bases[0];
    state->dispatcher.is_Node.ln_Pri = 100;
    state->collector.is_Node.ln_Pri = 127;
    state->dispatcher.is_Code = (APTR)dispatch;
    state->collector.is_Code = (APTR)collect;
    struct InterruptLock lock = lockInterrupts();
    AddIntServer(INTB_EXTER, &state->collector);
    AddIntServer(INTB_EXTER, &state->dispatcher);
    unlockInterrupts(lock);
    ready = TRUE;
done:

    if (platform) {
        DT_CloseKey(platform);
    }

    if (root) {
        DT_CloseKey(root);
    }

    if (!ready) {
        freeController(state);
    }

    return ready;
}

static struct PcInterruptBase* libOpen(ULONG version asm("d0"), struct PcInterruptBase* base asm("a6"))
{
    (void)version;
    ++base->library.lib_OpenCnt;
    base->state->bases[0]->library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    base->state->bases[1]->library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return base;
}

static ULONG libClose(struct PcInterruptBase* base asm("a6"))
{
    if (--base->library.lib_OpenCnt == 0 && (base->library.lib_Flags & LIBF_DELEXP)) {
        return libExpunge(base);
    }

    return 0;
}

static ULONG libNull(void)
{
    return 0;
}

// Task-context query. Controller domains are immutable while a library base is
// open. Decode through the original resource, without acquiring or changing IRQs.
static LONG resolveInterrupt(APTR controller asm("a0"), const UBYTE* specifier asm("a1"),
    ULONG cells asm("d0"), struct InterruptRoute* route asm("a2"), struct PcInterruptBase* base asm("a6"))
{
    if (!controller || !specifier || !route) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (!base->state) {
        return GIC400_ERR_NOT_READY;
    }

    APTR DeviceTreeBase = OpenResource("devicetree.resource");

    if (!DeviceTreeBase) {
        return GIC400_ERR_NOT_READY;
    }

    APTR root = DT_OpenKey((CONST_STRPTR)"/");

    if (!root) {
        return GIC400_ERR_DEVTREE;
    }

    struct DtInterrupt description = {controller, specifier, cells};
    LONG status = resolveInterruptDescription(DeviceTreeBase, root, &base->state->domains, &description, route);
    DT_CloseKey(root);
    return status;
}

static LONG allocMessages(ULONG count asm("d0"), struct InterruptMessage* messages asm("a0"),
    struct PcInterruptBase* base asm("a6"))
{
    if (!base || !base->state) {
        return GIC400_ERR_NOT_READY;
    }

    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();
    LONG result = allocateMessageInterrupts(&state->messages, &state->vectors, &state->local_apic,
        &state->pending, state->destination, count, messages);

    if (!result) {
        state->max_irqs = state->pending.max_irqs;

        for (ULONG i = 0; i < count; ++i) {
            state->config[messages[i].irq] = IRQ_EDGE;
            state->routes[messages[i].irq] = 0;
        }
    }

    unlockInterrupts(lock);
    return result;
}

static LONG freeMessages(ULONG first_irq asm("d0"), ULONG count asm("d1"), struct PcInterruptBase* base asm("a6"))
{
    if (!base || !base->state) {
        return GIC400_ERR_NOT_READY;
    }

    if (!count || count > 32 || (count & (count - 1)) || first_irq >= INTERRUPT_IRQ_LIMIT ||
        count > INTERRUPT_IRQ_LIMIT - first_irq) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct PcInterruptState* state = base->state;
    struct InterruptLock lock = lockInterrupts();
    LONG result = GIC400_ERR_NOT_READY;

    for (ULONG i = 0; i < count; ++i) {

        if (state->handlers[first_irq + i]) {
            goto done;
        }
    }

    result = freeMessageInterrupts(&state->messages, &state->vectors, &state->local_apic,
        &state->pending, first_irq, count);

    if (!result) {
        state->max_irqs = state->pending.max_irqs;

        for (ULONG i = 0; i < count; ++i) {
            state->config[first_irq + i] = state->routes[first_irq + i] = 0;
        }
    }

done:
    unlockInterrupts(lock);
    return result;
}

static const APTR function_table[] = {
    (APTR)libOpen, (APTR)libClose, (APTR)libExpunge, (APTR)libNull,
    (APTR)addServer, (APTR)remServer, (APTR)getStatus, (APTR)enableInt, (APTR)disableInt,
    (APTR)setPriority, (APTR)getPriority, (APTR)setEdge, (APTR)setLevel,
    (APTR)routeInt, (APTR)unrouteInt, (APTR)queryRoute, (APTR)setPending, (APTR)clearPending,
    (APTR)setActive, (APTR)clearActive, (APTR)setMask, (APTR)getMask, (APTR)getRunning,
    (APTR)getHighest, (APTR)getInfo, (APTR)setPolarity,
    (APTR)setDispatchEnabled, (APTR)collectInterrupts, (APTR)requestWakeup, (APTR)resolveInterrupt,
    (APTR)allocMessages, (APTR)freeMessages, (APTR)-1
};
// The original extension-ROM bootstrap initializes every mapped Resident. An
// early OS dependency may already have initialized this module; reuse it without
// allocating a second base, resetting IRQ state or linking the same node twice.
static struct Library* residentInit(ULONG seglist asm("a0"), struct ExecBase* exec_base asm("a6"))
{
    Forbid();
    struct Library* library = (void*)FindName(&exec_base->LibList, (STRPTR)library_name);

    if (!library && !FindName(&exec_base->LibList, (STRPTR)compatibility_name)) {
        struct PcInterruptState* state = AllocMem(sizeof(*state), MEMF_CLEAR);

        if (state) {
            state->seglist = seglist;
            state->dispatch_enabled = true;

            for (unsigned i = 0; i < 2; ++i) {
                struct PcInterruptBase* base = (void*)MakeLibrary((APTR)function_table, NULL, NULL,
                    sizeof(*base), seglist);

                if (!base) {
                    break;
                }

                state->bases[i] = base;
                base->state = state;
                base->library.lib_Node.ln_Type = NT_LIBRARY;
                base->library.lib_Node.ln_Name = (char*)(i ? compatibility_name : library_name);
                base->library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
                base->library.lib_Version = LIBRARY_VERSION;
                base->library.lib_Revision = LIBRARY_REVISION;
                base->library.lib_IdString = (STRPTR)(i ? compatibility_id : library_id);
            }

            if (state->bases[0] && state->bases[1] && initController(state)) {
                library = &state->bases[0]->library;
                AddLibrary(library);
                AddLibrary(&state->bases[1]->library);
            } else {

                for (unsigned i = 0; i < 2; ++i) {

                    if (state->bases[i]) {
                        freeBase(state->bases[i]);
                    }
                }

                FreeMem(state, sizeof(*state));
            }
        }
    }

    Permit();
    return library;
}
