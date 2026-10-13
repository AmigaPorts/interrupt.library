/* SPDX-License-Identifier: MPL-2.0 */
#include "devicetree_interrupt.h"

#include <stdint.h>
#define __NOLIBBASE__
#include <proto/devicetree.h>

static ULONG cell(const UBYTE* value)
{
    return ((ULONG)value[0] << 24) | ((ULONG)value[1] << 16) |
        ((ULONG)value[2] << 8) | value[3];
}

static BOOL propertyCell(APTR DeviceTreeBase, APTR property, ULONG* value)
{
    if (!property || DT_GetPropLen(property) != 4) {
	    return FALSE;
    }
	
    const UBYTE* bytes = DT_GetPropValue(property);
	
    if (!bytes) {
	    return FALSE;
    }
	
    *value = cell(bytes);
	
    return TRUE;
}

APTR dtFindPhandle(APTR DeviceTreeBase, APTR root, ULONG phandle)
{
    if (!DeviceTreeBase || !root || !phandle || phandle == UINT32_MAX) {
	    return NULL;
    }
	
    APTR key = root, found = NULL;
	
    do {
        APTR property = DT_FindProperty(key, (CONST_STRPTR)"phandle");
        APTR legacy = DT_FindProperty(key, (CONST_STRPTR)"linux,phandle");
        ULONG value = 0, old = 0;
    	
        if ((property && !propertyCell(DeviceTreeBase, property, &value)) ||
            (legacy && !propertyCell(DeviceTreeBase, legacy, &old)) ||
            (property && legacy && value != old)) {
	        return NULL;
        }
    	
        if ((property ? value : old) == phandle) {
            if (found) { return NULL; }
            found = key;
        }
    	
        APTR next = DT_GetChild(key, NULL);
    	
        while (!next && key != root) {
            APTR parent = DT_GetParent(key);
        	
            if (!parent) {
	            return NULL;
            }
        	
            next = DT_GetChild(parent, key);
            key = parent;
        }
    	
        key = next;
    } while (key);
	
    return found;
}

static APTR parentStep(APTR DeviceTreeBase, APTR root, APTR key)
{
    if (!key) {
	    return NULL;
    }
	
    APTR property = DT_FindProperty(key, (CONST_STRPTR)"interrupt-parent");
	
    if (!property) {
	    return DT_GetParent(key);
    }
	
    ULONG phandle;
	
    return propertyCell(DeviceTreeBase, property, &phandle) ?
        dtFindPhandle(DeviceTreeBase, root, phandle) : NULL;
}

static BOOL interruptCells(APTR DeviceTreeBase, APTR key, ULONG* cells)
{
    return key && propertyCell(DeviceTreeBase,
        DT_FindProperty(key, (CONST_STRPTR)"#interrupt-cells"), cells) && *cells;
}

static APTR interruptParent(APTR DeviceTreeBase, APTR root, APTR key)
{
    APTR slow = key, fast = key;
	
    for (;;) {
        slow = parentStep(DeviceTreeBase, root, slow);
    	
        if (!slow) {
	        return NULL;
        }
    	
        if (DT_FindProperty(slow, (CONST_STRPTR)"#interrupt-cells")) {
	        return slow;
        }
    	
        // Detect interrupt-parent cycles without allocating a visited-node list
        // or imposing an arbitrary limit on valid tree depth.
        for (unsigned i = 0; fast && i < 2; ++i) {
            fast = parentStep(DeviceTreeBase, root, fast);
        	
            if (fast && DT_FindProperty(fast, (CONST_STRPTR)"#interrupt-cells")) {
	            fast = NULL;
            }
        }
    	
        if (slow == fast) {
	        return NULL;
        }
    }
}

BOOL dtGetInterrupt(APTR DeviceTreeBase, APTR root, APTR key, ULONG index, struct DtInterrupt* interrupt)
{
    if (!DeviceTreeBase || !root || !key || !interrupt) {
	    return FALSE;
    }
	
    struct DtInterrupt result = {0};
    APTR property = DT_FindProperty(key, (CONST_STRPTR)"interrupts-extended");
	
    if (property) {
        const UBYTE* data = DT_GetPropValue(property);
        ULONG length = DT_GetPropLen(property);
    	
        if (!data || !length || length % 4) {
	        return FALSE;
        }
    	
        // Validate the complete list, including records following the selected
        // one. Each domain supplies its own specifier width.
        ULONG ordinal = 0;
    	
        for (ULONG offset = 0; offset < length;) {
            APTR domain = dtFindPhandle(DeviceTreeBase, root, cell(data + offset));
            ULONG cells;
        	
            if (!interruptCells(DeviceTreeBase, domain, &cells) || cells > (length - offset) / 4 - 1) {
                return FALSE;
            }
        	
            if (ordinal++ == index) {
                result = (struct DtInterrupt){domain, data + offset + 4, cells};
            }
        	
            offset += 4 * (cells + 1);
        }
    	
        if (!result.domain) {
	        return FALSE;
        }
    } else {
        property = DT_FindProperty(key, (CONST_STRPTR)"interrupts");
    	
        if (!property) {
	        return FALSE;
        }
    	
        const UBYTE* data = DT_GetPropValue(property);
        ULONG length = DT_GetPropLen(property), cells;
        APTR domain = interruptParent(DeviceTreeBase, root, key);
    	
        if (!data || !length || length % 4 || !interruptCells(DeviceTreeBase, domain, &cells) ||
            cells > length / 4 || (length / 4) % cells || index >= (length / 4) / cells) {
            return FALSE;
        }
    	
        result = (struct DtInterrupt){domain, data + 4 * index * cells, cells};
    }
	
    *interrupt = result;
	
    return TRUE;
}

struct InterruptRoute {
    struct DtInterrupt interrupt;
    const UBYTE* address;
    ULONG address_cells;
};

// Zero at a controller, one after mapping, -2 for no row, -1 for invalid input.
static int mapInterrupt(APTR DeviceTreeBase, APTR root, struct InterruptRoute* route)
{
    APTR domain = route->interrupt.domain;
    ULONG ic, ac;
	
    if (!domain || !route->interrupt.specifier || !interruptCells(DeviceTreeBase, domain, &ic) ||
        ic != route->interrupt.cells) {
	    return -1;
    }
	
    APTR property = DT_FindProperty(domain, (CONST_STRPTR)"interrupt-controller");
	
    if (property) {
	    return DT_GetPropLen(property) ? -1 : 0;
    }
	
    if (!propertyCell(DeviceTreeBase, DT_FindProperty(domain, (CONST_STRPTR)"#address-cells"), &ac) ||
        ac != route->address_cells || (ac && !route->address) ||
        ic >= UINT32_MAX / 4 || ac >= UINT32_MAX / 4 - ic) {
	    return -1;
    }
	
    ULONG child_cells = ac + ic;
    property = DT_FindProperty(domain, (CONST_STRPTR)"interrupt-map");
	
    if (!property) {
	    return -1;
    }
	
    const UBYTE* map = DT_GetPropValue(property);
    ULONG length = DT_GetPropLen(property);
	
    if ((length && !map) || length % 4) {
	    return -1;
    }
	
    const UBYTE* mask = NULL;
    property = DT_FindProperty(domain, (CONST_STRPTR)"interrupt-map-mask");
	
    if (property) {
        mask = DT_GetPropValue(property);
    	
        if (!mask || DT_GetPropLen(property) != 4 * child_cells) {
	        return -1;
        }
    }
	
    // This extension changes parent cells rather than selecting a table row.
    // Never silently return unmodified cells when such a transform is required.
    if (DT_FindProperty(domain, (CONST_STRPTR)"interrupt-map-pass-thru")) {
	    return -1;
    }
	
    struct InterruptRoute selected = {0};
	
    for (ULONG offset = 0; offset < length;) {
        ULONG available = (length - offset) / 4;
    	
        if (child_cells >= available) {
	        return -1;
        }
    	
        const UBYTE* entry = map + offset;
        APTR parent = dtFindPhandle(DeviceTreeBase, root, cell(entry + 4 * child_cells));
        ULONG pac, pic;
    	
        if (!parent || !interruptCells(DeviceTreeBase, parent, &pic) ||
            !propertyCell(DeviceTreeBase, DT_FindProperty(parent, (CONST_STRPTR)"#address-cells"), &pac)) {
           
        	return -1;
        }
    	
        available -= child_cells + 1;
    	
        if (pac > available || pic > available - pac) {
	        return -1;
        }
    	
        BOOL matches = TRUE;
    	
        for (ULONG i = 0; i < child_cells; ++i) {
            ULONG input = cell(i < ac ? route->address + 4 * i : route->interrupt.specifier + 4 * (i - ac));
            
        	if (mask) {
        		input &= cell(mask + 4 * i);
        	}
        	
            if (input != cell(entry + 4 * i)) {
	            matches = FALSE;
            }
        }
    	
        if (matches) {
            if (selected.interrupt.domain) {
	            return -1;
            }
        	
            selected.address = entry + 4 * (child_cells + 1);
            selected.address_cells = pac;
            selected.interrupt = (struct DtInterrupt){parent, selected.address + 4 * pac, pic};
        }
    	
        offset += 4 * (child_cells + 1 + pac + pic);
    }
	
    if (!selected.interrupt.domain) {
	    return -2;
    }
	
    *route = selected;
	
    return 1;
}

enum DtInterruptStatus dtResolveInterruptStatus(APTR DeviceTreeBase, APTR root, const struct DtInterrupt* interrupt,
    const UBYTE* unit_address, ULONG address_cells, struct DtInterrupt* result)
{
    if (!DeviceTreeBase || !root || !interrupt || !result) {
	    return DT_INTERRUPT_INVALID;
    }
	
    struct InterruptRoute slow = {*interrupt, unit_address, address_cells}, fast = slow;
    BOOL checking_cycle = TRUE, mapped = FALSE;
	
    for (;;) {
        int step = mapInterrupt(DeviceTreeBase, root, &slow);
    	
        if (step < 0) {
            return step == -2 && !mapped ? DT_INTERRUPT_MISSING : DT_INTERRUPT_INVALID;
        }
    	
        if (!step) {
            *result = slow.interrupt;
            return DT_INTERRUPT_FOUND;
        }
    	
        mapped = TRUE;
    	
        // Interrupt domains form a tree. Walk a second cursor twice as fast to
        // reject a cyclic map without allocations or a fixed nesting limit.
        for (unsigned i = 0; checking_cycle && i < 2; ++i) {
            step = mapInterrupt(DeviceTreeBase, root, &fast);
        	
            if (step < 0) {
	            return DT_INTERRUPT_INVALID;
            }
        	
            if (!step) {
	            checking_cycle = FALSE;
            }
        }
    	
        if (checking_cycle && slow.interrupt.domain == fast.interrupt.domain) {
	        return DT_INTERRUPT_INVALID;
        }
    }
}

BOOL dtResolveInterrupt(APTR tree_base, APTR root, const struct DtInterrupt* interrupt,
    const UBYTE* unit_address, ULONG address_cells, struct DtInterrupt* result)
{
    return dtResolveInterruptStatus(tree_base, root, interrupt, unit_address, address_cells, result) == DT_INTERRUPT_FOUND;
}
