# ScarlettVI console memory layer

**Status: experimental Xbox XMem heap and 64 KiB physical-page virtualization.**

## What works

The C++20 `src/runtime/physical_memory.*` code keeps a host-side registry of
synthetic physical page numbers. Each console 64 KiB page is backed by a
Windows pagefile-backed section object.

`MEM_RESERVE | MEM_RESERVE_PLACEHOLDER` reserves a contiguous virtual address
range; ScarlettVI splits this reservation into 64 KiB placeholders.
`MapViewOfFile3` with `MEM_REPLACE_PLACEHOLDER` maps specific physical
page sections at chosen virtual addresses. Multiple virtual views of the
same section genuinely share storage, without copying the bytes.
`UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` removes mappings safely.

The independently authored `ScarlettVI.exe memory` fixture verified:
- Allocate two 64 KiB physical pages with the publicly documented API pattern.
- Reserve three contiguous 64 KiB virtual page slots.
- Map two different pages into slots 0 and 1.
- Map a second alias of page 0 into slot 2.
- Write through the first virtual address and immediately read the same byte
  through the alias at the third address.
- Unmap, release the placeholder region, and free the physical backing.
- Confirm **zero leaked pages**.

ScarlettVI now provides *experimental* links for 10 XMem references involving
heap allocation, virtual allocation, physical-page allocation/free/map.
The title's host import of `VirtualFree` is transparently routed through
a local adapter which releases ScarlettVI-managed placeholder regions
correctly and delegates all other allocations to Windows.

## Known limits / warnings

- Synthetic page numbers are private identifiers in 4 KiB frame units;
  they are **not real physical addresses** and cannot be submitted to a GPU.
- Only 64 KiB page mode is implemented; 2 MiB clusters, scatter mappings,
  physical page transfer, and working-set statistics remain unsupported.
- Xbox memory allocation attributes are not exhaustively validated; the
  heap branch and ordinary virtual allocation branch remain experimental.
- The current virtual mapping path recognizes
  `MEM_RESERVE | MEM_64K_PAGES`; full graphics access/protection semantics
  are not implemented.
- Lifetime handling for dynamic imports and other DLL-created threads
  still requires further runtime hooks. The `VirtualFree` hook only changes
  the import table entries the custom loader actually patches.
- The test demonstrates host CPU memory aliases, **not Xbox GPU memory
  equivalence** or title runtime stability.
- Xbox SDK implementation-specific details are not included.

## Source documentation

- [Microsoft GDK sample: public allocation/mapping call sites](https://github.com/microsoft/Xbox-GDK-Samples/blob/main/Samples/Graphics/AMDFidelityFX_FSR3FrameInterpolation/FSR3FrameInterpolation.cpp)
- [Microsoft: VirtualAlloc2 placeholders](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2)
- [Microsoft: MapViewOfFile3](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3)
- [Microsoft: UnmapViewOfFile2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile2)
