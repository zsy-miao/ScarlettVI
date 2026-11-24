# Partial Xbox GPU virtual-memory configuration

**Status:** independently implemented Windows host CPU and GPU virtual-address
reservations for the only two configuration modes observed in the supplied
game. Both modes are verified on NVIDIA GeForce RTX 5060 Ti. **Xbox-specific
GPU page tables, physical tile mappings and coherency are not implemented.**

## Read-only call-site evidence

Independent MSVC disassembly of the user's
`game_scarlett_master.exe` at VA `0x141E1CFA0` through `0x141E1D048`
shows the following sequence:

1. Zero a **32-byte** stack configuration record.
2. Compute a range size of **16 GiB or 24 GiB**, depending on the program's
   selected memory mode.
3. Store either **3** or **4** in the DWORD at record offset **+0x04**,
   leaving every other byte zero.
4. Pass the record pointer in `RCX` to
   `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` at VA `0x141E1CFE0`;
   test the resulting HRESULT in `EAX`.
5. Reserve two separate virtual address windows of that same size using
   `xmem.dll!XMemVirtualAlloc`, with the 64 KiB reservation flags
   `0x20402000`, original attribute/protection combinations
   `(0x21, 4)` and `(0x3, 0x801)`.

This independently establishes the observed one-argument call ABI and the
configuration record's shape. **It does not establish the Xbox GPU driver's
full configuration semantics.** The low-level Windows memory flags are used
only where supported by the independent 64 KiB placeholder backend.

## Efficient sparse CPU reservations

`PhysicalMemory::reserve_alias_region` originally split an entire
reservation into 64 KiB Windows placeholders at creation. A single 24 GiB
region contains **393,216** page slots; this approach would need hundreds
of thousands of calls before the game could even map one page.

The revised backend uses exactly **one `VirtualAlloc2` placeholder**
per requested address window, without committing 16/24 GiB of RAM.
When a page is actually mapped, `split_placeholder_slot_unlocked`
isolates that slot with up to two
`VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)` calls,
then uses the existing `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`
to map a 64 KiB section object.

Unmapping preserves the original virtual slot. Releasing the region
uses Windows `VirtualQuery` to enumerate actual placeholder extents
instead of calling `VirtualFree` on every untouched 64 KiB page.
Existing scatter and contiguous alias operations use the same lazy splitter,
and retain ownership and alias-count tracking.

`ScarlettVI.exe memory-sparse` checks two **16 GiB** reservations, then two
**24 GiB** reservations. It maps and unmaps five sparse slots in each mode,
including physical-page aliases across the two huge address windows,
checks that distant unallocated pages remain `MEM_RESERVE`, and releases
all windows and physical pages. Both configurations passed in x64 Release.

## Real Windows D3D12 GPU virtual-address windows

`src/gpu/sparse_gpu_windows.{h,cpp}` creates genuine Windows
`ID3D12Resource` **reserved buffer resources** using
`ID3D12Device11::CreateReservedResource`, without backing those buffers
with committed GPU physical memory.

The module queries
`D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT` from the real
driver and verifies process and per-resource GPU VA address-width limits.
It registers the resulting native GPU virtual addresses in the original
thread-safe `GuestAddressSpace`, checks offset translations and
out-of-bounds queries, and unregisters every mapping on cleanup.

Measured host limits on NVIDIA GeForce RTX 5060 Ti:

| GPU virtual-address capability | Verified |
| --- | ---: |
| Bits per process | 44 |
| Bits per resource | 40 |
| Two simultaneously reserved 16 GiB GPU buffers | PASS |
| Two simultaneously reserved 24 GiB GPU buffers | PASS |
| Allocated physical VRAM for these reserved resources | 0 GiB |

**Do not submit shaders against these reserved resources yet.**
They have no mapped memory tiles and are not guaranteed readable or writable.
Unlike the earlier generated DXIL fixtures, this test is **GPU address-space
reservation only**, not a GPU computation or copy operation.

## Experimental `D3D12ConfigureVirtualMemoryX` bridge

`src/gpu/xbox_device_factory.cpp` implements a deliberately limited
one-argument, HRESULT-returning configuration bridge using the observed
32-byte record:

- Require offset +0x04 mode **3 or 4**, with all other bytes zero.
- Initialize the same persistent Windows Device11 backend used by the
  experimental `D3D12XboxCreateDevice` factory.
- Reserve **two real Windows D3D12 GPU virtual buffer windows** of 16 GiB
  for mode 3 or 24 GiB for mode 4; verify all registered address bounds.
- Publish configuration only once the actual GPU VA resources exist.
- Repeating the identical configuration is idempotent; an attempt to
  change the configured mode is rejected without discarding valid resources.
- A null, unknown, or nonzero-extended configuration is rejected
  with an error instead of a fabricated success result.

The corresponding Xbox import is now patched to the original adapter.
The user executable's read-only `readiness` report moves from 26 to
**25 unresolved Xbox import references**. The report and title execution
gate still explicitly mark **BLOCKED**.

Run the independent fixtures from the ScarlettVI project folder:

```powershell
.\build\x64\Release\ScarlettVI.exe memory-sparse
.\build\x64\Release\ScarlettVI.exe gpu-sparse-va
.\build\x64\Release\ScarlettVI.exe gpu-vmem-config 3
.\build\x64\Release\ScarlettVI.exe gpu-vmem-config 4
```

Each mode runs in a fresh process. The last two tests also request the
two large CPU windows via `XMemVirtualAlloc` after GPU configuration, as
the title's observed code does, and create a standard native
`ID3D12Device11` plus a DIRECT command queue.

## Critical remaining work

This does **not** make the Xbox GPU VA layout equal to the host GPU VA layout.
The native reserved resources are not automatically linked to XMem's CPU
pagefile sections, and the two address spaces retain distinct addresses.
To support the title, ScarlettVI still needs a correct Xbox tile-map
translator, pageable heap/address identity translation, residency and
fence/coherency handling, XG texture swizzle conversion and Xbox RDNA2
shader translation.

No game code, initialization callback or shader is executed in these tests,
and no source/content from the game's binaries is published.

Public Windows references:

- [D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_gpu_virtual_address_support)
- [ID3D12Device::CreateReservedResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource)
- [VirtualAlloc2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2)
- [VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)
