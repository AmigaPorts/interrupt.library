/* SPDX-License-Identifier: MPL-2.0 */
#include "local_apic.h"

#include <stddef.h>
#include <libraries/gic400.h>

#define LAPIC_VERSION 0x030
#define LAPIC_EOI     0x0b0
#define LAPIC_ISR     0x100
#define LAPIC_IRR     0x200
#define LAPIC_ICR     0x300
#define LAPIC_DELIVERY_PENDING (1u << 12)
#define LAPIC_DESTINATION_SELF (1u << 18)

static uint32_t littleLong(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static uint32_t readRegister(const struct LocalApic* apic, unsigned offset)
{
    // xAPIC requires aligned, 32-bit scalar transactions. Endian conversion
    // belongs here in the 68k driver, not in the emulator's MMIO access path.
    return littleLong(apic->registers[offset / sizeof(uint32_t)]);
}

int32_t openLocalApic(volatile void* address, uint32_t size, struct LocalApic* apic)
{
    uintptr_t base = (uintptr_t)address;

    if (!apic || !base || (base & 4095) || size < 0x400 || size - 1 > UINTPTR_MAX - base) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct LocalApic result = {address};
    unsigned version = readRegister(&result, LAPIC_VERSION) & 255;

    if (!version || version == 255) {
        return GIC400_ERR_NOT_READY;
    }

    *apic = result;
    return 0;
}

int32_t highestLocalApic(const struct LocalApic* apic, enum LocalApicBitmap bitmap)
{
    if (!apic || !apic->registers || (bitmap != LOCAL_APIC_PENDING && bitmap != LOCAL_APIC_IN_SERVICE)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    unsigned base = bitmap == LOCAL_APIC_PENDING ? LAPIC_IRR : LAPIC_ISR;

    for (unsigned bank = 8; bank-- > 0;) {
        uint32_t bits = readRegister(apic, base + 16 * bank);

        if (bits) {
            return 32 * bank + 31 - __builtin_clz(bits);
        }
    }

    return GIC400_ERR_NOT_FOUND;
}

int32_t localApicStatus(const struct LocalApic* apic, unsigned vector, bool* pending, bool* active)
{
    if (!apic || !apic->registers || vector > 255 || (!pending && !active)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    unsigned bank = (vector / 32) * 16;
    uint32_t bit = UINT32_C(1) << (vector & 31);

    if (pending) {
        *pending = !!(readRegister(apic, LAPIC_IRR + bank) & bit);
    }

    if (active) {
        *active = !!(readRegister(apic, LAPIC_ISR + bank) & bit);
    }

    return 0;
}

int32_t completeLocalApic(const struct LocalApic* apic, unsigned vector)
{
    if (vector < 16 || vector >= 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    int32_t active = highestLocalApic(apic, LOCAL_APIC_IN_SERVICE);

    if (active < 0) {
        return active;
    }

    if ((unsigned)active != vector) {
        return GIC400_ERR_NOT_FOUND;
    }

    apic->registers[LAPIC_EOI / sizeof(uint32_t)] = 0;
    return 0;
}

int32_t requestLocalApic(const struct LocalApic* apic, unsigned vector)
{
    if (!apic || !apic->registers || vector < 32 || vector >= 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (readRegister(apic, LAPIC_ICR) & LAPIC_DELIVERY_PENDING) {
        return GIC400_ERR_NOT_READY;
    }

    // Self shorthand ignores the destination field. One scalar low-word write
    // submits fixed delivery without disturbing the high destination register.
    apic->registers[LAPIC_ICR / sizeof(uint32_t)] = littleLong(LAPIC_DESTINATION_SELF | vector);
    return 0;
}
