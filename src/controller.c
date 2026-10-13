/* SPDX-License-Identifier: MPL-2.0 */
#include "controller.h"

#include <libraries/gic400.h>
#define __NOLIBBASE__
#include <proto/devicetree.h>

static BOOL propertyWord(APTR DeviceTreeBase, APTR key, const char* name, ULONG fallback, ULONG* result)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)name);

    if (!property) {
        *result = fallback;
        return TRUE;
    }

    const UBYTE* data = DT_GetPropValue(property);

    if (!data || DT_GetPropLen(property) != 4) {
        return FALSE;
    }

    *result = ((ULONG)data[0] << 24) | ((ULONG)data[1] << 16) | ((ULONG)data[2] << 8) | data[3];
    return TRUE;
}

LONG findApicWindow(APTR DeviceTreeBase, APTR root, enum ApicKind kind, APTR previous,
    struct ApicWindow* window)
{
    if (!DeviceTreeBase || !root || !window || (kind != APIC_LOCAL && kind != APIC_IO)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    const char* compatible = kind == APIC_LOCAL ? "intel,ce4100-lapic" : "intel,ce4100-ioapic";
    APTR key = dtFindCompatible(DeviceTreeBase, root, previous, compatible);

    if (!key) {
        return GIC400_ERR_NOT_FOUND;
    }

    APTR property = DT_FindProperty(key, (CONST_STRPTR)"interrupt-controller");
    ULONG cells;

    if (!property || DT_GetPropLen(property) ||
        !propertyWord(DeviceTreeBase, key, "#interrupt-cells", 0, &cells) || cells != 2) {
        return GIC400_ERR_DEVTREE;
    }

    // Both bindings describe exactly one register window. Validate its entire
    // tuple, including the parent bus's local (not inherited) cell counts.
    APTR parent = DT_GetParent(key);
    ULONG ac, sc;

    if (!parent || !propertyWord(DeviceTreeBase, parent, "#address-cells", 2, &ac) ||
        !propertyWord(DeviceTreeBase, parent, "#size-cells", 1, &sc) ||
        (ac != 1 && ac != 2) || (sc != 1 && sc != 2)) {
        return GIC400_ERR_DEVTREE;
    }

    property = DT_FindProperty(key, (CONST_STRPTR)"reg");
    struct ApicWindow result = {0};

    if (!property || DT_GetPropLen(property) != 4 * (ac + sc) ||
        !dtGetRegister(DeviceTreeBase, key, 0, &result.address, &result.size)) {
        return GIC400_ERR_DEVTREE;
    }

    // xAPIC uses a page-aligned MMIO block; IOREGSEL and IOWIN require aligned
    // 32-bit accesses. Validate only the extent needed by these register sets.
    ULONG alignment = kind == APIC_LOCAL ? 4096 : 4;
    ULONG minimum = kind == APIC_LOCAL ? 0x400 : 0x14;

    if (!result.address || (result.address & (alignment - 1)) || result.size < minimum) {
        return GIC400_ERR_DEVTREE;
    }

    if (kind == APIC_LOCAL) {
        property = DT_FindProperty(key, (CONST_STRPTR)"intel,virtual-wire-mode");

        if (property && DT_GetPropLen(property)) {
            return GIC400_ERR_DEVTREE;
        }

        result.virtual_wire = property != NULL;
    }

    result.key = key;
    *window = result;
    return 0;
}

LONG readInterruptCpus(APTR DeviceTreeBase, APTR platform, struct InterruptCpus* cpus)
{
    if (!DeviceTreeBase || !platform || !cpus) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    APTR property = DT_FindProperty(platform, (CONST_STRPTR)"cpu-apic-ids");

    if (!property) {
        return GIC400_ERR_NOT_FOUND;
    }

    ULONG length = DT_GetPropLen(property);
    const UBYTE* data = DT_GetPropValue(property);

    if (!data || !length || length > 32 || length % 4) {
        return GIC400_ERR_DEVTREE;
    }

    struct InterruptCpus result = {0};
    result.count = length / 4;

    for (ULONG i = 0; i < result.count; ++i) {
        const UBYTE* entry = data + 4 * i;
        ULONG id = ((ULONG)entry[0] << 24) | ((ULONG)entry[1] << 16) | ((ULONG)entry[2] << 8) | entry[3];

        if (id == 0xffffffffUL) {
            return GIC400_ERR_DEVTREE;
        }

        for (ULONG j = 0; j < i; ++j) {

            if (result.apic_ids[j] == id) {
                return GIC400_ERR_DEVTREE;
            }
        }

        result.apic_ids[i] = id;
    }

    *cpus = result;
    return 0;
}

LONG readInterruptVectors(APTR DeviceTreeBase, APTR platform, ULONG cpu, uint32_t available[8])
{
    if (!available) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct InterruptCpus cpus;
    LONG status = readInterruptCpus(DeviceTreeBase, platform, &cpus);

    if (status) {
        return status;
    }

    if (cpu >= cpus.count) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    APTR property = DT_FindProperty(platform, (CONST_STRPTR)"cpu-interrupt-vectors");

    if (!property) {
        return GIC400_ERR_NOT_FOUND;
    }

    ULONG length = DT_GetPropLen(property);
    const UBYTE* data = DT_GetPropValue(property);

    if (!data || length != cpus.count * 32) {
        return GIC400_ERR_DEVTREE;
    }

    uint32_t result[8];

    for (ULONG role = 0; role < cpus.count; ++role) {

        for (unsigned bank = 0; bank < 8; ++bank) {
            const UBYTE* entry = data + role * 32 + bank * 4;
            uint32_t word = ((uint32_t)entry[0] << 24) | ((uint32_t)entry[1] << 16) |
                ((uint32_t)entry[2] << 8) | entry[3];

            if ((!bank && word) || (bank == 7 && (word & UINT32_C(0x80000000)))) {
                return GIC400_ERR_DEVTREE;
            }

            if (role == cpu) {
                result[bank] = word;
            }
        }
    }

    for (unsigned bank = 0; bank < 8; ++bank) {
        available[bank] = result[bank];
    }

    return 0;
}
