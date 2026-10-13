/* SPDX-License-Identifier: MPL-2.0 */
#include "io_apic.h"

#include <libraries/gic400.h>

#define IOAPIC_ID      0x00
#define IOAPIC_VERSION 0x01
#define IOAPIC_ROUTE(n) (0x10 + 2 * (n))
#define IOAPIC_PENDING (1u << 12)
#define IOAPIC_REMOTE  (1u << 14)
#define IOAPIC_MASKED  (1u << 16)

static uint32_t littleLong(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static uint32_t readRegister(const struct IoApic* apic, unsigned index)
{
    apic->registers[0] = littleLong(index);
    return littleLong(apic->registers[4]);
}

static void writeRegister(const struct IoApic* apic, unsigned index, uint32_t value)
{
    apic->registers[0] = littleLong(index);
    apic->registers[4] = littleLong(value);
}

int32_t openIoApic(volatile void* address, uint32_t size, struct IoApic* apic)
{
    uintptr_t base = (uintptr_t)address;

    if (!apic || !base || (base & 3) || size < 0x14 || size - 1 > UINTPTR_MAX - base) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct IoApic result = { .registers = address };
    uint32_t version = readRegister(&result, IOAPIC_VERSION);
    result.version = version & 255;
    result.input_count = ((version >> 16) & 255) + 1;

    if (!result.version || result.version == 255 || result.input_count > IOAPIC_INPUTS_MAX) {
        return GIC400_ERR_NOT_READY;
    }

    result.id = readRegister(&result, IOAPIC_ID) >> 24;
    *apic = result;
    return 0;
}

static int32_t validatePin(const struct IoApic* apic, unsigned pin)
{
    if (!apic || !apic->registers || !apic->input_count || apic->input_count > IOAPIC_INPUTS_MAX) {
        return GIC400_ERR_NOT_READY;
    }

    return pin < apic->input_count ? 0 : GIC400_ERR_INVALID_IRQ;
}

int32_t ioApicState(const struct IoApic* apic, unsigned pin, struct IoApicState* state)
{
    if (!state) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    int32_t result = validatePin(apic, pin);

    if (result) {
        return result;
    }

    unsigned reg = IOAPIC_ROUTE(pin);
    uint32_t high = readRegister(apic, reg + 1);
    uint32_t low = readRegister(apic, reg);
    *state = (struct IoApicState) {
        .destination = high >> 24, .vector = low & 255, .delivery_mode = (low >> 8) & 7,
        .logical_destination = !!(low & (1u << 11)), .active_low = !!(low & (1u << 13)),
        .level_triggered = !!(low & (1u << 15)), .masked = !!(low & IOAPIC_MASKED),
        .delivery_pending = !!(low & IOAPIC_PENDING), .remote_irr = !!(low & IOAPIC_REMOTE)
    };
    return 0;
}

int32_t routeIoApic(const struct IoApic* apic, unsigned pin, unsigned vector,
    unsigned destination, bool active_low, bool level_triggered)
{
    if (vector < 32 || vector >= 255 || destination >= 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    int32_t result = validatePin(apic, pin);

    if (result) {
        return result;
    }

    unsigned reg = IOAPIC_ROUTE(pin);
    uint32_t low = readRegister(apic, reg);
    writeRegister(apic, reg, low | IOAPIC_MASKED);
    low = readRegister(apic, reg);

    if ((low & IOAPIC_PENDING) || ((low & (1u << 15)) && (low & IOAPIC_REMOTE))) {
        return GIC400_ERR_NOT_READY;
    }

    // Hardware owns delivery status and remote IRR. Preserve reserved fields,
    // replace the destination with the old input masked, then the low half.
    uint32_t high = readRegister(apic, reg + 1);
    writeRegister(apic, reg + 1, (high & 0x00ffffff) | ((uint32_t)destination << 24));
    low = (low & ~0xafffu) | IOAPIC_MASKED | vector |
        ((uint32_t)active_low << 13) | ((uint32_t)level_triggered << 15);
    writeRegister(apic, reg, low);
    (void)readRegister(apic, reg);
    return 0;
}

int32_t maskIoApic(const struct IoApic* apic, unsigned pin, bool masked)
{
    int32_t result = validatePin(apic, pin);

    if (result) {
        return result;
    }

    unsigned reg = IOAPIC_ROUTE(pin);
    uint32_t low = readRegister(apic, reg);

    if (!masked) {
        uint32_t high = readRegister(apic, reg + 1);

        if ((low & 0xf00) || (low & 255) < 32 || (low & 255) == 255 || (high >> 24) == 255) {
            return GIC400_ERR_NOT_ROUTABLE;
        }
    }

    writeRegister(apic, reg, masked ? low | IOAPIC_MASKED : low & ~IOAPIC_MASKED);
    (void)readRegister(apic, reg);
    return 0;
}
