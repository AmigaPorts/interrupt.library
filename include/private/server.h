/* SPDX-License-Identifier: MPL-2.0 */
// Callback convention follows rondoval's gic400.library under MPL-2.0.
// Source revision and license option are recorded in README.md.
#pragma once
#include <exec/execbase.h>
#include <exec/interrupts.h>

// The caller validates is_Code and keeps the server alive through entry. The
// callback receives IRQ in D0, is_Data in A1 and ExecBase in A6; return ignored.
static inline void callInterruptServer(struct Interrupt* interrupt, ULONG irq, struct ExecBase* exec_base)
{
    register ULONG irq_value asm("d0") = irq;
    register APTR data asm("a1") = interrupt->is_Data;
    register struct ExecBase* sys_base asm("a6") = exec_base;
    __asm__ volatile("jsr (%[code])"
        : "+r"(irq_value), "+r"(data), "+r"(sys_base)
        : [code] "a"(interrupt->is_Code)
        : "d1", "a0", "a5", "cc", "memory");
}

