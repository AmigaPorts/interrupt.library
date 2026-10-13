/* SPDX-License-Identifier: MPL-2.0 */
#include "domain.h"

#include <libraries/gic400.h>
#define __NOLIBBASE__
#include <proto/devicetree.h>

#include "controller.h"

static ULONG cell(const UBYTE* value)
{
    return ((ULONG)value[0] << 24) | ((ULONG)value[1] << 16) | ((ULONG)value[2] << 8) | value[3];
}

LONG readIoApicGsiBase(APTR DeviceTreeBase, APTR root, APTR platform, APTR controller, ULONG* gsi_base)
{
    if (!DeviceTreeBase || !root || !platform || !controller || !gsi_base) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    APTR property = DT_FindProperty(platform, (CONST_STRPTR)"ioapic-gsi-bases");

    if (!property) {
        return GIC400_ERR_NOT_FOUND;
    }

    ULONG length = DT_GetPropLen(property);
    const UBYTE* data = DT_GetPropValue(property);

    if (length % 8 || (length && !data)) {
        return GIC400_ERR_DEVTREE;
    }

    BOOL found = FALSE;
    ULONG result = 0;

    for (ULONG offset = 0; offset < length; offset += 8) {
        ULONG handle = cell(data + offset), base = cell(data + offset + 4);
        APTR key = dtFindPhandle(DeviceTreeBase, root, handle);

        if (!key) {
            return GIC400_ERR_DEVTREE;
        }

        for (ULONG previous = 0; previous < offset; previous += 8) {

            if (cell(data + previous) == handle || cell(data + previous + 4) == base) {
                return GIC400_ERR_DEVTREE;
            }
        }

        // Match by identity, never by position. Discovery also checks status,
        // controller binding and translated register extent without reading MMIO.
        struct ApicWindow window;
        APTR previous = NULL;
        do {
            LONG status = findApicWindow(DeviceTreeBase, root, APIC_IO, previous, &window);

            if (status) {
                return GIC400_ERR_DEVTREE;
            }

            previous = window.key;
        } while (previous != key);

        if (key == controller) {
            found = TRUE;
            result = base;
        }
    }

    if (!found) {
        return GIC400_ERR_NOT_FOUND;
    }

    *gsi_base = result;
    return 0;
}
