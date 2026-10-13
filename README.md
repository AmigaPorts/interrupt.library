# interrupt.library
[![CMake](https://github.com/amigaports/interrupt.library/actions/workflows/cmake.yml/badge.svg)](https://github.com/amigaports/interrupt.library/actions/workflows/cmake.yml)

Native PC interrupt-controller support for AmigaOS 3.x running on Emu68.
The library discovers hardware through `devicetree.resource` and manages
local APIC and I/O APIC interrupts from the 68k side.

It provides interrupt handlers, routing, priorities, masking, trigger/polarity
control and message-interrupt allocation. The current backend supports xAPIC
MMIO; x2APIC is not supported.

## Compatibility

`gic400.library` is a runtime compatibility alias. Both names share controller
state and handlers. Existing GIC API vectors, register conventions, structures
and error codes are preserved; PC extensions follow the original vectors.
New applications should use `interrupt.library` and `<proto/interrupt.h>`.

## Building

Requires CMake, Git, `xxd`, and an `m68k-amigaos` cross-toolchain with AmigaOS
headers and `sfdc`. From a configured cross-toolchain environment:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom
cmake --build build --parallel
```

CPM fetches the pinned build helpers and public `devicetree.resource` SDK.
Outputs include `build/interrupt.library`, generated SDK bindings and
`build/interrupt.library.h` for ROM embedding. Runtime initialization requires
`devicetree.resource` and a compatible PC hardware description.

## License

[Mozilla Public License 2.0](LICENSE). The public ABI and Resident/lifecycle
layout derive from [rondoval's gic400.library](https://github.com/rondoval/emu68-gic400-library/tree/d2f7f586020cce9de815d3753e6a7b69edecf236),
using its MPL-2.0 license option. The PC controller implementation is independent.
