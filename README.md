<div align="center">

<img src="assets/scarlettvi-logo.png" alt="ScarlettVI — neon sunset wordmark" width="520" />

# ScarlettVI

**A custom, C++20 native Xbox Series X|S compatibility layer for Windows.**

[Architecture](docs/ARCHITECTURE.md) · [Startup Readiness](docs/STARTUP_READINESS.md) · [GPU](docs/GPU_BACKEND.md) · [Device11 Bridge](docs/XBOX_DEVICE11.md) · [Sparse GPU VM](docs/GPU_VIRTUAL_MEMORY.md) · [Real GPU Tiles](docs/GPU_TILES.md) · [XG Texture Research](docs/XG_TEXTURE_LAYOUT.md) · [CPU/GPU XMem Staging](docs/XMEM_GPU_STAGING.md) · [GPU Page Stager API](docs/GPU_PAGE_STAGER.md) · [Pinned GPU Frame Registry](docs/GUEST_GPU_PAGE_REGISTRY.md) · [GPU Memory](docs/GPU_ADDRESS_BRIDGE.md) · [Memory](docs/MEMORY_BACKEND.md) · [Scatter Mapping](docs/XMEM_SCATTER.md) · [Assets](docs/ASSET_FORMATS.md) · [Source](src)

</div>

---

ScarlettVI is an original **Visual Studio 2026 / C++** research and engineering
project investigating how to run Xbox Series X|S x64 game executables on a
Windows PC. Rather than CPU emulation, the goal is to build a dedicated PE
loader, runtime ABI bridge, Xbox operating-system compatibility layer and
graphics translation pipeline.

The primary compatibility target is an Xbox Series build of Grand Theft Auto VI.
This repository **does not include game files or Xbox proprietary components**.
You must supply your own legitimately obtained software.

## Status

**Early development — not bootable or playable yet.**

The native x64 PE mapper now supports recursive DLL dependencies, forwarded
exports, host import routing, original XMem HLE, section protections, initial
TLS and x64 unwind registration. On the current target, it binds 341/341
executable Windows imports, 489/489 nested Windows imports and 315/315
title-local exports. **25 Xbox-specific import references remain unresolved.**
The new `readiness` mode reports every missing symbol, owning module, IAT
address and remaining process-initialization milestone. Execution is blocked.

The original experimental 64 KiB physical memory manager supports actual
host memory aliasing through Windows placeholders, with a `VirtualFree` bridge.
An experimental `XMemVirtualQuery` bridge uses an ABI independently derived
from read-only disassembly of the bundled Xbox BVH module, with host memory
state queried through Windows (see [analysis](docs/XMEM_VIRTUAL_QUERY.md)).
The original `XMemMapPhysicalPagesScatter` bridge now supports validated
**non-contiguous 64 KiB page mappings across multiple reserved regions**, with
real aliases and safe unmapping (see [call-site research](docs/XMEM_SCATTER.md)).
An experimental [Xbox Device11 factory bridge](docs/XBOX_DEVICE11.md) now
recognizes the title's exact requested Windows `ID3D12Device11` GUID, and
returns a real native COM interface for the independently observed factory
parameters. The [partial Xbox GPU VA configuration bridge](docs/GPU_VIRTUAL_MEMORY.md)
now reserves real 16/24 GiB Windows GPU virtual ranges and supports the game's
huge 64 KiB-granular CPU virtual reservations through lazy placeholder splitting.
Xbox-only tile mappings, GPU page tables, coherency and creation flags remain unsupported.
The Windows D3D12 backend successfully initializes a real graphics queue,
command list, upload resource, fence and root signature on an RTX 5060 Ti.
The persistent Windows D3D12 context now executes GPU texture copies and
compute shaders. Independently authored HLSL is compiled to DXIL Shader Model
6 using the installed Windows SDK, with real SRV/UAV buffer and texture bindings.
GPU tests verified **2,048 computed integers** and **16,384 generated RGBA8
texture pixels** after GPU execution on an RTX 5060 Ti. The new original
[GPU memory bridge](docs/GPU_ADDRESS_BRIDGE.md) translates synthetic guest
GPU addresses into D3D12 resources and executes DXIL compute directly from
ScarlettVI-managed 64 KiB physical-page data; another guest alias verifies
**2,048 GPU results** with no leaked mappings.
The original [native GPU tile mapper](docs/GPU_TILES.md) now segments the
logical 16/24 GiB guest GPU windows into 4 GiB Windows D3D12 resources.
A real 64 KiB GPU heap tile is mapped into two distant guest locations;
an aliasing-barrier and readback test verified **65,536/65,536 bytes** on
the RTX 5060 Ti. The new original [host texture-layout bridge](docs/XG_TEXTURE_LAYOUT.md)
verifies RGBA8, R8, BC1, BC3 and BC5 block/pitch translation: **130 generated CPU
subresources (211,460 bytes)** and **40 real GPU texture subresources** passed
across mip levels and arrays. Xbox GPU page tables and XG swizzles are not translated.
The original [XMem-to-sparse-GPU staging test](docs/XMEM_GPU_STAGING.md)
now uses a reusable [GpuPageStager host API](docs/GPU_PAGE_STAGER.md) to
transfer **three different 64 KiB payloads** through real sparse GPU tiles
across **two separate GPU sessions**. It explicitly writes back to a CPU
alias and verifies both views, including GPU resource segment-boundary
positions, invalid-input rejection and **zero leaked pages**. This **does not** provide unified CPU/GPU memory or the
Xbox `XMemTransferMemory` ABI.

The new [pinned guest GPU page registry](docs/GUEST_GPU_PAGE_REGISTRY.md)
tracks four real XMem-backed 64 KiB frame IDs, eight pinned CPU aliases
and eight real D3D12 tile mappings. The registry now uses **one shared
D3D12 command queue**, submitting multi-page command lists together behind
one batch fence: **12 verified 64 KiB GPU transfers in 6 native queue
submissions**. It rejects invalid frame/slot associations and pinned unmaps,
then releases all GPU mappings, CPU pins and physical frames without leaks.
This is host resource lifetime management, **not Xbox GPU page tables**.

These are verified host capabilities; the **Xbox-specific D3D12/XG device ABI,
RDNA2 shader translation and actual title rendering are still missing**.

The `inspect`, `map` and `archives` modes are read-only with respect to the
game and never execute its entry point or initializers.

## Build

- Windows 10/11 x64
- Visual Studio 2026 with the C++ desktop workload (MSVC v145)
- Windows SDK with DirectX Shader Compiler (DXC), required for GPU shader modes
- Open `ScarlettVI.sln`, choose `x64` + `Release`, then Build Solution.

Or build from a Developer PowerShell:

```powershell
msbuild ScarlettVI.sln /p:Configuration=Release /p:Platform=x64
```

## Inspect a title (read-only)

```powershell
.\build\x64\Release\ScarlettVI.exe inspect "D:\Games\Title\title.exe" --json "reports\imports.json"
.\build\x64\Release\ScarlettVI.exe map "D:\Games\Title\title.exe"
.\build\x64\Release\ScarlettVI.exe readiness "D:\Games\Title\title.exe" --json "reports\readiness.json"
.\build\x64\Release\ScarlettVI.exe archives "D:\Games\Title"
.\build\x64\Release\ScarlettVI.exe gpu
.\build\x64\Release\ScarlettVI.exe gpu-transfer
.\build\x64\Release\ScarlettVI.exe gpu-compute
.\build\x64\Release\ScarlettVI.exe gpu-texture-shader
.\build\x64\Release\ScarlettVI.exe gpu-texture-layout
.\build\x64\Release\ScarlettVI.exe gpu-memory-bridge
.\build\x64\Release\ScarlettVI.exe gpu-device11
.\build\x64\Release\ScarlettVI.exe gpu-device-factory
.\build\x64\Release\ScarlettVI.exe gpu-page-registry
.\build\x64\Release\ScarlettVI.exe gpu-xmem-tiles
.\build\x64\Release\ScarlettVI.exe gpu-tile-map
.\build\x64\Release\ScarlettVI.exe gpu-sparse-va
.\build\x64\Release\ScarlettVI.exe gpu-vmem-config 3
.\build\x64\Release\ScarlettVI.exe gpu-vmem-config 4
.\build\x64\Release\ScarlettVI.exe memory-sparse
.\build\x64\Release\ScarlettVI.exe memory
.\build\x64\Release\ScarlettVI.exe memory-scatter
```

The `map` mode maps the image into process memory, applies relocations,
links Windows and available title-local imports, prepares TLS and registers
unwind metadata. **It does not execute title code.** The `archives` command
reads RPF9 container metadata only (first 64 bytes of each top-level archive),
not its encrypted directory or contents. The `readiness` mode generates a
machine-readable blocker inventory and **does not execute the title**.

See [architecture](docs/ARCHITECTURE.md) and
[asset format research](docs/ASSET_FORMATS.md) for implementation details.

## Independence and acknowledgements

The project is independently authored. We reviewed the public architecture
and status descriptions of [XCL](https://github.com/xcl-dev/xcl) and
[VIctory](https://github.com/Marvisak/VIctory) for general context, without
copying their code, binary libraries or title files.

## Legal

ScarlettVI does not provide a copy of Grand Theft Auto VI, console firmware,
restricted SDK headers, keys or proprietary DLLs. Game and console brands
remain trademarks of their respective owners. Do not distribute game assets
through issues or pull requests.
