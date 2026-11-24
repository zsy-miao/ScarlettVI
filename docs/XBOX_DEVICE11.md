# Partial Xbox D3D12 device-creation compatibility

**Status:** experimental Xbox-shaped factory ABI bound to a real Windows
`ID3D12Device11` COM object. Not sufficient to boot or render the game.

## Independently verified interface identity

Read-only MSVC disassembly of the user's original
`game_scarlett_master.exe` at VA `0x1424785B7` reveals a call through
`d3d12_xs.dll!D3D12XboxCreateDevice`, using the Windows x64 four-argument
calling convention:

| Argument | Game-initialized value |
| --- | --- |
| RCX | null adapter |
| RDX | stack-local Xbox device-configuration structure |
| R8 | pointer to GUID at game VA `0x1461E0638` |
| R9 | pointer to a global device-interface output slot |

The GUID's 16 file bytes are
`44 c3 05 54 57 d4 4e 44 b4 dd 23 66 e4 5a ee 39`;
these unambiguously represent **5405c344-d457-444e-b4dd-2366e45aee39**.
The installed Microsoft Windows 10 SDK `d3d12.h` identifies this as
`ID3D12Device11` (inherited from `ID3D12Device10`).
A different nearby GUID at game VA `0x1461E0648` is
`0ec870a6-5d7e-4c22-8cfc-5baae07616ed`, which the same public SDK
identifies as `ID3D12CommandQueue`.

The caller initializes version `0x00020004` at offset 0 and
`0x00100000` (1 MiB) at offset 8 of its configuration object. It also writes
additional Xbox-specific configuration fields near offsets +0x20, +0x24,
+0x28 and +0x3C, whose full semantics are **not verified**.

Microsoft's public GDK examples use the same four-argument factory and name
fields such as `Version`, `GraphicsCommandQueueRingSizeBytes`,
`ProcessDebugFlags`, and shader-allocation controls. ScarlettVI reuses
**no GDK source or restricted console SDK headers**.

## Implemented independent C++20 bridge

`src/gpu/xbox_device_factory.{h,cpp}` implements an explicitly partial
adapter:

```cpp
HRESULT WINAPI xbox_create_device_host(
    void* adapter,
    const XboxDeviceParametersObserved* configuration,
    REFIID requested_iid,
    void** output);
```

It is wired only to the matching title import name.
The adapter:

- Validates the output pointer and clears it on error.
- Requires a null adapter (the only caller pattern independently observed).
- Requires the exact standard `ID3D12Device11` GUID.
- Accepts only the observed SDK version and graphics queue-ring size.
- Lazily initializes the original persistent Windows D3D12 backend.
- Uses `QueryInterface` to return an actual Windows `ID3D12Device11`
  with a separate COM reference owned by the caller.
- Rejects unsupported adapter, GUID and version/ring-size combinations
  rather than returning a fake success object.

**Important:** The Xbox-only configuration bytes beyond the verified
version/ring-size are not translated or honored by the Windows driver.
The bridge currently exposes only the standard Windows COM semantics.
It cannot promise that console-specific methods, fences, memory heaps,
device extensions, ring allocations or resource flags work correctly.
A successful factory result therefore does **not** mean title startup is
safe; the independent readiness gates still block execution.

## Verification

`ScarlettVI.exe gpu-device11` verifies that the native Windows runtime
on NVIDIA RTX 5060 Ti actually exposes the requested public COM interface,
and validates real queue creation, allocation-info method behavior and COM
identity through `IUnknown`.

`ScarlettVI.exe gpu-device-factory` invokes the new Xbox ABI-shaped factory
with independently generated configuration bytes and verifies valid creation,
native queue methods, stable COM identity, independently owned references,
and expected failures for null parameters/output, non-null console adapters,
unrecognized GUIDs, and unsupported version/queue values.

**Observed:** both commands returned `PASS` in the VS2026 x64 Release
build. A read-only import map of the real executable reports **one new
experimental device-factory binding**. The later experimental
[GPU VM configuration bridge](GPU_VIRTUAL_MEMORY.md) links one more reference,
leaving **25** unresolved Xbox import references. Game code was not executed.

## Still missing

- `D3D12ConfigureVirtualMemoryX` now has a **partial host reservation bridge**;
  the console GPU's actual virtual page-map and physical-tile policies remain
  unsupported (see [GPU virtual memory](GPU_VIRTUAL_MEMORY.md)).
- Xbox-only device/queue/resource COM interface extensions and GPU VA mappings.
- GPU tiling, heap residency, resource flags, RDNA2 shader ISA to DXIL
  conversion, presentation and game lifecycle services.

## Public documentation

- [Microsoft GDK sample showing D3D12XboxCreateDevice](https://github.com/microsoft/Xbox-GDK-Samples/blob/main/Samples/Live/InGameStore/DeviceResources.cpp)
- [Microsoft Windows ID3D12Device API reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12device)
- [Microsoft Windows SDK d3d12.h interface definitions](https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3d12.h)

No game binary, proprietary GDK header, firmware or external emulator code is
included in ScarlettVI.
