/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <exec/types.h>

// Borrowed keys/data from the original resource; no private tree representation.
// Specifier cells retain device-tree big-endian encoding and domain semantics.
struct DtInterrupt {
    APTR domain;
    const UBYTE* specifier;
    ULONG cells;
};

// A phandle is a tree-wide identity, not a node's position in enumeration.
// Reject reserved handles, conflicting legacy aliases and duplicate identities.
APTR dtFindPhandle(APTR tree_base, APTR root, ULONG phandle);

// Decode a device's indexed interrupt in its immediate interrupt domain.
// interrupts-extended takes precedence. Ordinary interrupts follow explicit or
// inherited interrupt-parent links. Failure leaves output unchanged. A domain
// may be an interrupt nexus: its interrupt-map must then be resolved separately;
// callers must not interpret nexus cells as controller IRQ numbers.
BOOL dtGetInterrupt(APTR tree_base, APTR root, APTR key, ULONG index, struct DtInterrupt* interrupt);

// Follow interrupt-map entries until reaching the controller for this source.
// Unit address cells are big-endian, as in reg. Parent address/specifier widths
// come from each map row's phandle. Reject incomplete, ambiguous or cyclic maps.
// Returned data is borrowed from the tree (or input for an existing controller);
// failure leaves output unchanged. Controller cascades are handled by drivers.
BOOL dtResolveInterrupt(APTR tree_base, APTR root, const struct DtInterrupt* interrupt,
    const UBYTE* unit_address, ULONG address_cells, struct DtInterrupt* result);

// A valid initial nexus with no matching row is distinct from invalid routing.
// Once a row matches, an unresolved parent is an error, not missing wiring.
// This is a client helper, not an additional devicetree.resource vector.
enum DtInterruptStatus { DT_INTERRUPT_INVALID = -1, DT_INTERRUPT_MISSING, DT_INTERRUPT_FOUND };
enum DtInterruptStatus dtResolveInterruptStatus(APTR tree_base, APTR root,
    const struct DtInterrupt* interrupt, const UBYTE* unit_address, ULONG address_cells,
    struct DtInterrupt* result);
