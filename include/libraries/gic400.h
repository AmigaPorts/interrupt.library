// SPDX-License-Identifier: MPL-2.0
// From rondoval/emu68-gic400-library, used under its MPL-2.0 option; see README.md.
#ifndef LIBRARIES_GIC400_H
#define LIBRARIES_GIC400_H

#include <exec/types.h>

/* Public GIC-400 API status codes. Functions return 0 on success or one of
 * these negative values on failure.
 */
#define GIC400_ERR_NOT_READY ((LONG)-1)
#define GIC400_ERR_INVALID_IRQ ((LONG)-2)
#define GIC400_ERR_INVALID_ARGUMENT ((LONG)-3)
#define GIC400_ERR_NOT_ROUTABLE ((LONG)-4)
#define GIC400_ERR_ALREADY_REGISTERED ((LONG)-5)
#define GIC400_ERR_NOT_FOUND ((LONG)-6)
#define GIC400_ERR_NO_MEMORY ((LONG)-7)
#define GIC400_ERR_DEVTREE ((LONG)-8)

struct GICInfo
{
    ULONG distributorIIDR;
    ULONG distributorTyper;
    ULONG cpuInterfaceIIDR;
    ULONG maxIrqs;
    UBYTE cpuCount;
    UBYTE securityExtensions;
    UBYTE lspiCount;
};

// PC extension: a resolved descriptor, not an acquired interrupt. The caller
// still registers/configures the source with the existing interrupt API.
struct InterruptRoute {
    ULONG irq;
    BOOL edge, active_low;
};

// PC extension: native 68k integers describing a controller-owned message.
struct InterruptMessage {
    ULONG irq;
    ULONG address_low, address_high;
    ULONG data;
};

#endif /* LIBRARIES_GIC400_H */
