/* SPDX-License-Identifier: MPL-2.0 */
#include "controller.h"

#include <libraries/gic400.h>

LONG readIoApicInterrupt(APTR DeviceTreeBase, APTR root, const struct DtInterrupt* interrupt,
    struct IoApicInterrupt* result)
{
    if (!DeviceTreeBase || !root || !interrupt || !interrupt->domain || !interrupt->specifier || !result) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (interrupt->cells != 2) {
        return GIC400_ERR_DEVTREE;
    }

    struct IoApicInterrupt decoded = {0};
    APTR previous = NULL;
    do {
        LONG status = findApicWindow(DeviceTreeBase, root, APIC_IO, previous, &decoded.controller);

        if (status) {
            return status;
        }

        previous = decoded.controller.key;
    } while (previous != interrupt->domain);

    // The controller binding defines each cell's meaning. Byte loads also work
    // for unaligned specifiers returned through the public device-tree resource.
    const UBYTE* cells = interrupt->specifier;
    decoded.pin = ((ULONG)cells[0] << 24) | ((ULONG)cells[1] << 16) | ((ULONG)cells[2] << 8) | cells[3];
    ULONG sense = ((ULONG)cells[4] << 24) | ((ULONG)cells[5] << 16) | ((ULONG)cells[6] << 8) | cells[7];

    switch (sense) {
        case 0:
            break;
        case 1:
            decoded.active_low = TRUE;
            decoded.level_triggered = TRUE;
            break;
        case 2:
            decoded.level_triggered = TRUE;
            break;
        case 3:
            decoded.active_low = TRUE;
            break;
        default:
            return GIC400_ERR_DEVTREE;
    }

    *result = decoded;
    return 0;
}
