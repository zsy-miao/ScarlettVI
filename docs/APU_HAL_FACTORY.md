# Partial AcpHal factory and guest-visible vtable

**Status: partially implemented host object. NOT a working Xbox DSP device.** The named `AcpHal.dll!AcpHalCreate` import now returns a real allocated object, but its unverified DSP-configuration method returns `E_NOTIMPL` and other unobserved vtable slots remain unavailable. Xbox title execution is still BLOCKED.

## Evidence from read-only title disassembly

At `0x1443AED91`, the title supplies the address of a 64-bit global pointer to `AcpHalCreate`, checks EAX for a failed HRESULT, then uses the returned guest object's first vtable pointer:

- `0x1443AEDBA`: slot `+0x00`, `this`, `EDX=0x1000`, `R8D=0x1000`; an HRESULT is checked.
- `0x1443AEE2F`: slot `+0x10`, `this`, `EDX=2`, `R8D=0xF0F0F0F0`, `R9D=0xFFFFFFFF`, stack argument points to a DWORD with value `0xC0`, final stack DWORD is zero. The caller checks the returned HRESULT.
- `0x1443AE84F`: slot `+0x08` used at shutdown; no explicit additional arguments.
- `0x1443AE88B`: slot `+0x28` used after shutdown; no explicit additional arguments.

The callsites establish register/stack argument shapes, method offset and return checks; they do **not** establish Xbox processing semantics, interface identity, refcount policy, or undocumented vtable slots `+0x18` and `+0x20`.

## Original implementation

`src/audio/acp_hal_host.{h,cpp}` implements the four observed method positions and a pointer-sized guest vtable. `AcpHalCreate` allocates an owned host object and writes the output pointer only after success. The first method allocates two real zeroed Windows CPU ring buffers up to 1 MiB each, validates requested sizes, permits idempotent reinitialization and returns BUSY for incompatible live reconfiguration. The stop method releases the buffers; the method at `+0x28` destroys the object and its remaining resources. Resource accounting tracks live objects, buffers and bytes with atomics.

The `+0x10` method deliberately returns `E_NOTIMPL` without reading or modifying the caller's data: no verified audio/DSP operation exists for the supplied magic values. Unobserved method slots remain null rather than fabricated. A legitimate title may abort initialization upon `E_NOTIMPL`; do not claim that this implementation enables boot or audio.

The exact factory binding is separately audited as `HAL factory partial=1` in console readiness, and JSON field `experimental_apu_hal_partial_bound`. It is **not** counted among the eight functional host APU heap, address and SHAPE-storage bindings.

## Generated tests

    .\build\x64\Release\ScarlettVI.exe audio-hal-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

Fixture checks ABI signatures, vtable slot offsets (`0x00`, `0x08`, `0x10`, `0x28`), null-output rejection, two independently allocated objects, actual writable zeroed ring buffers, bounds and BUSY behavior, idempotent init/stop, explicitly unsupported DSP configuration, cleanup and 4 concurrent worker threads ×128 iterations. It does not execute proprietary title instructions.

## Readiness delta

| | Before | After |
| --- | ---: | ---: |
| Pending `AcpHal.dll` addresses | 1 | **0** |
| Pending all Xbox addresses | 15 | **14** |
| Partial HAL factory addresses | 0 | **1** |
| Guest execution gate | BLOCKED | BLOCKED |

Remaining Xbox imports: XAPU 5, CHAD 5, XG 2, DirectStorage 1, XMemTransferMemory 1. Even with these resolved, the original console GPU shader translator, runtime initialization and virtual file system/asset pipeline are incomplete.

No third-party implementation source was copied. Background context: the upstream VIctory and ztrx projects expose full audio runtimes and shader translation, but their README performance claims have not been reproduced in ScarlettVI.
