# XMem transfer and working-set call sites; staged host GPU-page bridge

**Status:** independent read-only executable call-site investigation, plus an original verified Windows CPU/GPU staging fixture. The three Xbox calls described below are **not patched or claimed implemented**. ScarlettVI continues to block all title execution.

## Read-only call evidence

The user's executable uses x64 import trampolines in its main PE. The following were independently identified using the existing import inventory and MSVC dumpbin without running or modifying the executable:

| Xbox import | Call VA | Observed register inputs/result |
| --- | --- | --- |
| `xmem.dll!XMemIsToolsMemAvailable` | `0x142215FD7` | The caller explicitly prepares no arguments; checks `EAX` for nonzero success and otherwise enters a diagnostic trap |
| `xmem.dll!XMemGetWorkingSetStatistics` | `0x142216008`, `0x142216042` | `RCX=0x2000000A`, `RDX=address of a zero-initialized stack output buffer` with at least `0x78` bytes initialized; `EAX` checked for nonzero, then output fields are compared |
| `xmem.dll!XMemGetWorkingSetStatistics` | `0x14247454E` | `RCX=0x20000001`, `RDX=address of stack output`; caller immediately reads two 64-bit output fields |
| `xmem.dll!XMemTransferMemory` | `0x14221608E` | `RCX=1`, `RDX=min(16 MiB, round_up(delta, 2 MiB))`; `EAX` checked for nonzero success |

The two working-set selectors use different output consumers. Their full struct definitions, authorization/quota semantics and error behavior are **not established** by these calls. `XMemTransferMemory` is a **memory-budget/ownership-related console API** in Microsoft's public documentation, not a proven byte-copy function. There is therefore no justification to implement it with `memcpy` or a generic success stub.

Microsoft's public GDK reference marks these Xbox memory interfaces as authorization-restricted. The [XMemIsToolsMemAvailable reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xmem/functions/xmemistoolsmemavailable) and the [XLaunchNewGame reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgame/functions/xlaunchnewgame) give public context but do **not** establish this title's complete XMem ABI. No restricted documentation, proprietary system DLL or NDA library is needed for the independent host experiment below.

## Independently implemented CPU/GPU staging

ScarlettVI already has two separate real Windows memory backends:

- **CPU side:** 64 KiB pagefile-backed `CreateFileMappingW` sections, reserved placeholder addresses and `MapViewOfFile3` aliases in `PhysicalMemory`/`xmem_bridge`.
- **GPU side:** real D3D12 `ID3D12Heap` tiles, `UpdateTileMappings` and 4 GiB segmented reserved-buffer resources in `SparseGpuWindows`.

These two host allocations do **not** refer to one unified physical page. Their memory is not automatically coherent. The original `DeviceContext::staged_xmem_tile_roundtrip_test` therefore exercises an explicit, independently controlled transfer pipeline:

1. Create one real CPU XMem physical-page section and reserve two 64 KiB CPU virtual regions, mapping the section into both. Verify that generated writes through one address are visible at the second.
2. Reserve two **logical 16 GiB guest GPU VA windows** with the original 4 GiB native-segment workaround, and map **one real GPU default-heap 64 KiB tile** into guest offsets **4 GiB** and **12 GiB**, on two distinct host D3D12 resources.
3. Copy all **65,536 generated CPU bytes** into a Windows upload resource, issue a GPU copy into the first reserved resource, execute a D3D12 resource-alias barrier, and copy from the second reserved resource into a readback resource. Fence before checking every byte.
4. Independently generate a second, different 64 KiB payload; write it to the GPU heap tile and read it back through the second GPU alias. Confirm that **both CPU aliases still contain the original bytes** before any explicit host-side writeback; they are not unified memory.
5. After fence completion and exact GPU byte verification, explicitly copy the GPU readback into the second CPU alias and verify that the first CPU alias sees **all updated bytes**, proving the CPU pagefile-section alias is intact.
6. Enqueue NULL GPU tile mappings and fence them, release GPU resources, CPU placeholder aliases and physical frame, then assert **zero leaked allocated physical pages**.

Verified on **NVIDIA GeForce RTX 5060 Ti** in x64 Release: **PASS**, 2 GPU command-list submissions, completed fence **4**, `HRESULT=0`, 65,536-byte payloads checked across each stage, **0 leaked CPU pages**.

The new test deliberately does **not** call or bind Xbox `XMemTransferMemory`; the word *transfer* here describes only staged host copies. The test also never executes game code, uses only independently generated data, and does not translate a console GPU physical page table.

## Reproduce

```powershell
.\build\x64\Release\ScarlettVI.exe gpu-xmem-tiles
.\build\x64\Debug\ScarlettVI.exe gpu-xmem-tiles
```

Future work needs verified console page descriptors, resource residency and memory-budget contracts before this staged host backend can safely back a real Xbox memory-management call. The read-only readiness report must continue to show the three XMem symbols as pending.