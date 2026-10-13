/* SPDX-License-Identifier: MPL-2.0 */
#include "devicetree_address.h"
#include <stdint.h>
#define __NOLIBBASE__
#include <proto/devicetree.h>

static uint32_t cell(const UBYTE* value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
        ((uint32_t)value[2] << 8) | value[3];
}

static uint64_t number(const UBYTE* value, unsigned cells)
{
    return cells == 2 ? ((uint64_t)cell(value) << 32) | cell(value + 4) : cell(value);
}

static unsigned cells(APTR DeviceTreeBase, APTR key, const char* name, unsigned fallback)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)name);
	
    if (!property) {
	    return fallback;
    }
	
    const UBYTE* value = DT_GetPropValue(property);
	
    if (!value || DT_GetPropLen(property) != 4) {
	    return 0;
    }
	
    unsigned count = cell(value);
	
    return count == 1 || count == 2 ? count : 0;
}

static BOOL matches(const UBYTE* value, ULONG length, const char* expected)
{
    if (!value || !expected) {
	    return FALSE;
    }
	
    ULONG offset = 0;
    BOOL found = FALSE;
	
    while (offset < length) {
        ULONG start = offset;
    	
        while (offset < length && value[offset]) {
	        ++offset;
        }
    	
        if (offset == length) {
	        return FALSE;
        }
    	
        ULONG i = 0;
    	
        while (i < offset - start && expected[i] && expected[i] == value[start + i]) {
	        ++i;
        }
    	
        if (i == offset - start && !expected[i]) {
	        found = TRUE;
        }
    	
        ++offset;
    }
	
    return found;
}

static BOOL enabled(APTR DeviceTreeBase, APTR key)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)"status");
	
    if (!property) {
	    return TRUE;
    }
	
    ULONG length = DT_GetPropLen(property);
    const UBYTE* value = DT_GetPropValue(property);
	
    return (length == 3 && matches(value, length, "ok")) ||
        (length == 5 && matches(value, length, "okay"));
}

APTR dtFindCompatible(APTR DeviceTreeBase, APTR root, APTR previous, const char* compatible)
{
    if (!DeviceTreeBase || !root || !compatible || !*compatible) {
	    return NULL;
    }
	
    APTR key = previous ? previous : root;
	
    if (previous) {
        APTR parent = previous;
    	
        while (parent && parent != root) {
            parent = DT_GetParent(parent);
        	
            if (parent && !enabled(DeviceTreeBase, parent)) {
	            return NULL;
            }
        }
    	
        if (!parent) {
	        return NULL;
        }
    }
	
    BOOL first = !previous;
	
    for (;;) {
        BOOL available = enabled(DeviceTreeBase, key);
    	
        if (!first) {
            APTR child = available ? DT_GetChild(key, NULL) : NULL;
        	
            while (!child && key != root) {
                APTR parent = DT_GetParent(key);
            	
                if (!parent) {
	                return NULL;
                }
            	
                child = DT_GetChild(parent, key);
                key = parent;
            }
        	
            if (!child) {
	            return NULL;
            }
        	
            key = child;
            available = enabled(DeviceTreeBase, key);
        }
    	
        first = FALSE;
    	
        if (!available) {
	        continue;
        }
    	
        APTR property = DT_FindProperty(key, (CONST_STRPTR)"compatible");
    	
        if (property && matches(DT_GetPropValue(property), DT_GetPropLen(property), compatible)) {
	        return key;
        }
    }
}

BOOL dtGetRegister(APTR DeviceTreeBase, APTR key, ULONG index, ULONG* address, ULONG* size)
{
    if (!DeviceTreeBase || !key || !address || !size) {
	    return FALSE;
    }
	
    APTR bus = DT_GetParent(key);
	
    if (!bus) {
	    return FALSE;
    }
	
    unsigned ac = cells(DeviceTreeBase, bus, "#address-cells", 2);
    unsigned sc = cells(DeviceTreeBase, bus, "#size-cells", 1);
    APTR property = DT_FindProperty(key, (CONST_STRPTR)"reg");
	
    if (!ac || !sc || !property) {
	    return FALSE;
    }
	
    const UBYTE* value = DT_GetPropValue(property);
    ULONG length = DT_GetPropLen(property), stride = 4 * (ac + sc);
	
    if (!value || length % stride || index >= length / stride) {
	    return FALSE;
    }
	
    value += index * stride;
    uint64_t base = number(value, ac), span = number(value + 4 * ac, sc);
    ULONG translated;
	
    if (!dtTranslateAddress(DeviceTreeBase, bus, base, span, &translated)) {
	    return FALSE;
    }
	
    *address = translated;
    *size = span;
	
    return TRUE;
}

BOOL dtTranslateAddress(APTR DeviceTreeBase, APTR bus, uint64_t base, uint64_t span, ULONG* address)
{
    if (!DeviceTreeBase || !bus || !address) {
	    return FALSE;
    }
	
    unsigned ac = cells(DeviceTreeBase, bus, "#address-cells", 2);
    unsigned sc = cells(DeviceTreeBase, bus, "#size-cells", 1);
	
    if (!ac || !sc) {
	    return FALSE;
    }
	
    if (!span || span - 1 > UINT64_MAX - base ||
        (ac == 1 && (base > UINT32_MAX || span - 1 > UINT32_MAX - base))) {
	    return FALSE;
    }
	
    for (;;) {
        APTR parent = DT_GetParent(bus);
    	
        if (!parent) {
	        break;
        }
    	
        unsigned pac = cells(DeviceTreeBase, parent, "#address-cells", 2);
        APTR property = DT_FindProperty(bus, (CONST_STRPTR)"ranges");
    	
        if (!pac || !property) {
	        return FALSE;
        }
    	
        const UBYTE* value = DT_GetPropValue(property);
        ULONG length = DT_GetPropLen(property);
    	
        if (length) {
            ULONG stride = 4 * (ac + pac + sc);
        	
            if (!value || length % stride) {
	            return FALSE;
            }
        	
            BOOL found = FALSE;
            uint64_t translated = 0;
        	
            for (ULONG offset = 0; offset < length; offset += stride) {
                uint64_t child = number(value + offset, ac);
                uint64_t host = number(value + offset + 4 * ac, pac);
                uint64_t window = number(value + offset + 4 * (ac + pac), sc);
            	
                if (!window || window - 1 > UINT64_MAX - child || window - 1 > UINT64_MAX - host ||
                    (ac == 1 && window - 1 > UINT32_MAX - child) ||
                    (pac == 1 && window - 1 > UINT32_MAX - host)) {
                    return FALSE;
                }
            	
                if (base >= child && base - child < window && span <= window - (base - child)) {
                    if (found) { return FALSE; }
                    translated = host + (base - child);
                    found = TRUE;
                }
            }
        	
            if (!found) {
	            return FALSE;
            }
        	
            base = translated;
        }
    	
        // An empty ranges property is an identity mapping; an absent one is
        // deliberately rejected above. Cell counts are local, not inherited.
        if (pac == 1 && (base > UINT32_MAX || span - 1 > UINT32_MAX - base)) {
	        return FALSE;
        }
    	
        bus = parent;
        ac = pac;
        sc = cells(DeviceTreeBase, bus, "#size-cells", 1);
    	
        if (!sc) {
	        return FALSE;
        }
    }
	
    if (base > UINT32_MAX || span > UINT32_MAX || span - 1 > UINT32_MAX - base) { return FALSE; }
    *address = base;
	
    return TRUE;
}
