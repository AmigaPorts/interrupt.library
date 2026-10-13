/* SPDX-License-Identifier: MPL-2.0 */
#include "domain.h"
#include "controller.h"
#include <libraries/gic400.h>

LONG resolveInterruptDescription(APTR tree_base, APTR root, const struct InterruptDomains* domains,
    const struct DtInterrupt* description, struct InterruptRoute* route)
{
    if (!route) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (!domains || (domains->count && !domains->domains)) {
        return GIC400_ERR_NOT_READY;
    }

    struct IoApicInterrupt decoded;
    LONG status = readIoApicInterrupt(tree_base, root, description, &decoded);

    if (status) {
        return status;
    }

    struct InterruptRoute pending;
    status = findInterruptIrq(domains, decoded.controller.key, decoded.pin, &pending.irq);

    if (status) {
        return status;
    }

    pending.edge = !decoded.level_triggered;
    pending.active_low = decoded.active_low;
    *route = pending;
    return 0;
}
