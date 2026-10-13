/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <exec/types.h>
#include <stdint.h>

// Client helpers using only the public devicetree.resource vectors. No access
// to the resource's private tree layout or Emu68 internals.
APTR dtFindCompatible(APTR tree_base, APTR root, APTR previous, const char* compatible);

// Decode a memory-mapped reg entry and translate through its ancestor ranges.
// Supports ordinary 1/2-cell buses. PCI child addresses require the PCI binding
// and belong in pcie.library. Reject unmapped or non-68k-addressable intervals;
// on failure, leave both outputs untouched. Returned values are guest addresses.
BOOL dtGetRegister(APTR tree_base, APTR key, ULONG index, ULONG* address, ULONG* size);

// Translate an interval already decoded in an ordinary bus address space. The
// bus is the node whose children use that space. This shares the same ancestor
// and 32-bit result checks as dtGetRegister; failure leaves address untouched.
BOOL dtTranslateAddress(APTR tree_base, APTR bus, uint64_t base, uint64_t size, ULONG* address);
